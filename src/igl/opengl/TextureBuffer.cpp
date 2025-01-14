/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <igl/opengl/TextureBuffer.h>

#include <array>
#include <igl/opengl/Errors.h>
#include <utility>

namespace igl {
namespace opengl {

namespace {
// maps TextureCube::CubeFace to GL target type for cube map faces
// required for glTexImageXXX APIs
constexpr std::array<GLenum, 6> kCubeFaceTargets = {GL_TEXTURE_CUBE_MAP_POSITIVE_X,
                                                    GL_TEXTURE_CUBE_MAP_NEGATIVE_X,
                                                    GL_TEXTURE_CUBE_MAP_POSITIVE_Y,
                                                    GL_TEXTURE_CUBE_MAP_NEGATIVE_Y,
                                                    GL_TEXTURE_CUBE_MAP_POSITIVE_Z,
                                                    GL_TEXTURE_CUBE_MAP_NEGATIVE_Z};
void swapTextureChannelsForFormat(igl::opengl::IContext& context,
                                  GLuint target,
                                  igl::TextureFormat iglFormat) {
  if (iglFormat == igl::TextureFormat::A_UNorm8 &&
      context.deviceFeatures().hasInternalRequirement(
          InternalRequirement::SwizzleAlphaTexturesReq)) {
    if (iglFormat == igl::TextureFormat::A_UNorm8) {
      // In GL3, GL_RED is used since GL_ALPHA is removed. To keep parity, red value must be set
      // to the alpha channel.
      context.texParameteri(target, GL_TEXTURE_SWIZZLE_R, GL_ZERO);
      context.texParameteri(target, GL_TEXTURE_SWIZZLE_G, GL_ZERO);
      context.texParameteri(target, GL_TEXTURE_SWIZZLE_B, GL_ZERO);
      context.texParameteri(target, GL_TEXTURE_SWIZZLE_A, GL_RED);
    }
  }
}
} // namespace

TextureBuffer::~TextureBuffer() {
  GLuint textureID = getId();
  if (textureID != 0) {
    if (textureHandle_ != 0) {
      getContext().makeTextureHandleNonResident(textureHandle_);
    }
    getContext().deleteTextures({textureID});
  }
}

uint64_t TextureBuffer::getTextureId() const {
  if (textureHandle_ == 0) {
    textureHandle_ = getContext().getTextureHandle(getId());
    IGL_ASSERT(textureHandle_);
    getContext().makeTextureHandleResident(textureHandle_);
  }
  return textureHandle_;
}

// create a 2D texture given the specified dimensions and format
Result TextureBuffer::create(const TextureDesc& desc, bool hasStorageAlready) {
  Result result = Super::create(desc, hasStorageAlready);
  if (result.isOk()) {
    const auto isSampledOrStorage = (desc.usage & (TextureDesc::TextureUsageBits::Sampled |
                                                   TextureDesc::TextureUsageBits::Storage)) != 0;
    if (isSampledOrStorage || desc.type != TextureType::TwoD || desc.numMipLevels > 1) {
      result = createTexture(desc);
    } else {
      result = Result(Result::Code::Unsupported, "invalid usage!");
    }
  }
  return result;
}

void TextureBuffer::bindImage(size_t unit) {
  // The entire codebase used only combined kShaderRead|kShaderWrite access (except tests)
  // @fb-only
  // Here we used to have this condition:
  //    getUsage() & TextureUsage::kShaderWrite ? GL_WRITE_ONLY : GL_READ_ONLY,
  // So it is safe to replace it with GL_READ_WRITE
  IGL_ASSERT_MSG(getUsage() & TextureDesc::TextureUsageBits::Storage, "Should be a storage image");
  getContext().bindImageTexture((GLuint)unit,
                                getId(),
                                0,
                                getTarget() == GL_TEXTURE_2D ? GL_TRUE : GL_FALSE,
                                0,
                                GL_READ_WRITE,
                                glInternalFormat_);
}

// create a texture for shader read/write usages
Result TextureBuffer::createTexture(const TextureDesc& desc) {
  const auto target = toGLTarget(desc.type, desc.numSamples);
  if (target == 0) {
    return Result(Result::Code::Unsupported, "Unsupported texture target");
  }

  // If usage doesn't include Storage, ensure usage includes sampled for correct format selection
  const auto usageForFormat = (desc.usage & TextureDesc::TextureUsageBits::Storage) == 0
                                  ? desc.usage | TextureDesc::TextureUsageBits::Sampled
                                  : desc.usage;
  if (!toFormatDescGL(desc.format, usageForFormat, formatDescGL_)) {
    // can't create a texture with the given format
    return Result(Result::Code::ArgumentInvalid, "Invalid texture format");
  }

  if (!getProperties().isCompressed() && formatDescGL_.type == GL_NONE) {
    return Result(Result::Code::ArgumentInvalid, "Invalid texture type");
  }

  if (desc.usage & TextureDesc::TextureUsageBits::Storage) {
    if (!getContext().deviceFeatures().hasInternalFeature(InternalFeatures::TexStorage)) {
      return Result(Result::Code::Unsupported, "Texture Storage not supported");
    }
  }

  glInternalFormat_ = formatDescGL_.internalFormat;

  // create the GL texture ID
  GLuint textureID;
  getContext().genTextures(1, &textureID);

  setTextureBufferProperties(textureID, target);
  setUsage(desc.usage);

  if (desc.type == TextureType::ExternalImage) {
    // No further initialization needed for external image textures
    return Result{};
  } else {
    return initialize();
  }
}

Result TextureBuffer::initialize() const {
  const auto target = getTarget();
  if (target == 0) {
    return Result{Result::Code::InvalidOperation, "Unknown texture type"};
  }
  getContext().bindTexture(target, getId());
  setMaxMipLevel();
  if (getNumMipLevels() == 1) { // Change default min filter to ensure mipmapping is disabled
    getContext().texParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  }
  if (!getProperties().isCompressed()) {
    swapTextureChannelsForFormat(getContext(), target, getFormat());
  }
  Result result;
  if (canInitialize()) {
    if (!supportsTexStorage()) {
      result = initializeWithUpload();
    } else {
      result = initializeWithTexStorage();
    }
  }

  getContext().bindTexture(getTarget(), 0);
  return result;
}

Result TextureBuffer::initializeWithUpload() const {
  const auto target = getTarget();
  for (size_t i = 0; i < getNumMipLevels(); ++i) {
    const auto range = getFullRange(i);
    auto result = upload(target, range, nullptr);
    if (!result.isOk()) {
      return result;
    }
  }
  return Result{};
}

Result TextureBuffer::initializeWithTexStorage() const {
  const auto range = getFullRange(0, getNumMipLevels());
  const auto target = getTarget();
  switch (getType()) {
  case TextureType::TwoD:
    getContext().texStorage2D(
        target, range.numMipLevels, glInternalFormat_, (GLsizei)range.width, (GLsizei)range.height);
    break;
  case TextureType::TwoDArray:
    getContext().texStorage3D(target,
                              range.numMipLevels,
                              glInternalFormat_,
                              (GLsizei)range.width,
                              (GLsizei)range.height,
                              (GLsizei)range.numLayers);
    break;
  case TextureType::ThreeD:
    getContext().texStorage3D(target,
                              range.numMipLevels,
                              glInternalFormat_,
                              (GLsizei)range.width,
                              (GLsizei)range.height,
                              (GLsizei)range.depth);
    break;
  case TextureType::Cube:
    getContext().texStorage2D(
        target, range.numMipLevels, glInternalFormat_, (GLsizei)range.width, (GLsizei)range.height);
    break;
  default:
    IGL_ASSERT_MSG(false, "Unknown texture type");
    return Result{Result::Code::InvalidOperation, "Unknown texture type"};
  }
  return getContext().getLastError();
}

Result TextureBuffer::upload1D(GLenum target,
                               const TextureRangeDesc& range,
                               const void* data) const {
  const auto result = validateRange(range);
  if (!result.isOk()) {
    return result;
  }
  // Use TexImage when range covers full texture AND texture was not initialized with TexStorage
  const auto texImage = isValidForTexImage(range) && !supportsTexStorage();
  if (data == nullptr || !getProperties().isCompressed()) {
    if (texImage) {
      getContext().texImage1D(target,
                              (GLsizei)range.mipLevel,
                              formatDescGL_.internalFormat,
                              (GLsizei)range.width,
                              0, // border
                              formatDescGL_.format,
                              formatDescGL_.type,
                              data);
    } else {
      getContext().texSubImage1D(target,
                                 (GLsizei)range.mipLevel,
                                 (GLsizei)range.x,
                                 (GLsizei)range.width,
                                 formatDescGL_.format,
                                 formatDescGL_.type,
                                 data);
    }
  } else {
    const auto numCompressedBytes = getProperties().getBytesPerRange(range);
    IGL_ASSERT(numCompressedBytes > 0);
    if (texImage) {
      getContext().compressedTexImage1D(target,
                                        (GLint)range.mipLevel,
                                        formatDescGL_.internalFormat,
                                        (GLsizei)range.width,
                                        0, // border
                                        (GLsizei)numCompressedBytes, // TODO: does not
                                                                     // work for
                                                                     // compressed mipmaps
                                        data);
    } else {
      getContext().compressedTexSubImage1D(getTarget(),
                                           (GLint)range.mipLevel,
                                           (GLint)range.x,
                                           (GLsizei)range.width,
                                           formatDescGL_.internalFormat,
                                           (GLsizei)numCompressedBytes, // TODO: does not work
                                                                        // for compressed
                                                                        // mipmaps
                                           data);
    }
  }

  return getContext().getLastError();
}

Result TextureBuffer::upload1DArray(GLenum target,
                                    const TextureRangeDesc& range,
                                    const void* data) const {
  const auto result = validateRange(range);
  if (!result.isOk()) {
    return result;
  }
  // Use TexImage when range covers full texture AND texture was not initialized with TexStorage
  const auto texImage = isValidForTexImage(range) && !supportsTexStorage();
  if (data == nullptr || !getProperties().isCompressed()) {
    if (texImage) {
      getContext().texImage2D(target,
                              (GLsizei)range.mipLevel,
                              formatDescGL_.internalFormat,
                              (GLsizei)range.width,
                              (GLsizei)range.numLayers,
                              0, // border
                              formatDescGL_.format,
                              formatDescGL_.type,
                              data);
    } else {
      getContext().texSubImage2D(target,
                                 (GLsizei)range.mipLevel,
                                 (GLsizei)range.x,
                                 (GLsizei)range.layer,
                                 (GLsizei)range.width,
                                 (GLsizei)range.numLayers,
                                 formatDescGL_.format,
                                 formatDescGL_.type,
                                 data);
    }
  } else {
    const auto numCompressedBytes = getProperties().getBytesPerRange(range);
    IGL_ASSERT(numCompressedBytes > 0);
    if (texImage) {
      getContext().compressedTexImage2D(target,
                                        (GLint)range.mipLevel,
                                        formatDescGL_.internalFormat,
                                        (GLsizei)range.width,
                                        (GLsizei)range.numLayers,
                                        0, // border
                                        (GLsizei)numCompressedBytes, // TODO: does not
                                                                     // work for
                                                                     // compressed mipmaps
                                        data);
    } else {
      getContext().compressedTexSubImage2D(getTarget(),
                                           (GLint)range.mipLevel,
                                           (GLint)range.x,
                                           (GLint)range.layer,
                                           (GLsizei)range.width,
                                           (GLsizei)range.numLayers,
                                           formatDescGL_.internalFormat,
                                           (GLsizei)numCompressedBytes,
                                           data);
    }
  }

  return getContext().getLastError();
}

Result TextureBuffer::upload2D(GLenum target,
                               const TextureRangeDesc& range,
                               const void* data) const {
  const auto result = validateRange(range);
  if (!result.isOk()) {
    return result;
  }
  // Use TexImage when range covers full texture AND texture was not initialized with TexStorage
  const auto texImage = isValidForTexImage(range) && !supportsTexStorage();
  if (data == nullptr || !getProperties().isCompressed()) {
    if (texImage) {
      getContext().texImage2D(target,
                              (GLsizei)range.mipLevel,
                              formatDescGL_.internalFormat,
                              (GLsizei)range.width,
                              (GLsizei)range.height,
                              0, // border
                              formatDescGL_.format,
                              formatDescGL_.type,
                              data);
    } else {
      getContext().texSubImage2D(target,
                                 (GLsizei)range.mipLevel,
                                 (GLsizei)range.x,
                                 (GLsizei)range.y,
                                 (GLsizei)range.width,
                                 (GLsizei)range.height,
                                 formatDescGL_.format,
                                 formatDescGL_.type,
                                 data);
    }
  } else {
    const auto numCompressedBytes = getProperties().getBytesPerRange(range);
    IGL_ASSERT(numCompressedBytes > 0);
    if (texImage) {
      getContext().compressedTexImage2D(target,
                                        (GLint)range.mipLevel,
                                        formatDescGL_.internalFormat,
                                        (GLsizei)range.width,
                                        (GLsizei)range.height,
                                        0, // border
                                        (GLsizei)numCompressedBytes, // TODO: does not work
                                                                     // for compressed
                                                                     // mipmaps
                                        data);
    } else {
      getContext().compressedTexSubImage2D(getTarget(),
                                           (GLint)range.mipLevel,
                                           (GLint)range.x,
                                           (GLint)range.y,
                                           (GLsizei)range.width,
                                           (GLsizei)range.height,
                                           formatDescGL_.internalFormat,
                                           (GLsizei)numCompressedBytes, // TODO: does not work
                                                                        // for compressed
                                                                        // mipmaps
                                           data);
    }
  }
  return getContext().getLastError();
}
Result TextureBuffer::upload2DArray(GLenum target,
                                    const TextureRangeDesc& range,
                                    const void* data) const {
  const auto result = validateRange(range);
  if (!result.isOk()) {
    return result;
  }
  // Use TexImage when range covers full texture AND texture was not initialized with TexStorage
  const auto texImage = isValidForTexImage(range) && !supportsTexStorage();
  if (data == nullptr || !getProperties().isCompressed()) {
    if (texImage) {
      getContext().texImage3D(target,
                              (GLint)range.mipLevel,
                              formatDescGL_.internalFormat,
                              (GLsizei)range.width,
                              (GLsizei)range.height,
                              (GLsizei)range.numLayers,
                              0, // border
                              formatDescGL_.format,
                              formatDescGL_.type,
                              data);
    } else {
      getContext().texSubImage3D(target,
                                 (GLsizei)range.mipLevel,
                                 (GLsizei)range.x,
                                 (GLsizei)range.y,
                                 (GLsizei)range.layer,
                                 (GLsizei)range.width,
                                 (GLsizei)range.height,
                                 (GLsizei)range.numLayers,
                                 formatDescGL_.format,
                                 formatDescGL_.type,
                                 data);
    }
  } else {
    const auto numCompressedBytes = getProperties().getBytesPerRange(range);
    IGL_ASSERT(numCompressedBytes > 0);
    if (texImage) {
      getContext().compressedTexImage3D(target,
                                        (GLint)range.mipLevel,
                                        formatDescGL_.internalFormat,
                                        (GLsizei)range.width,
                                        (GLsizei)range.height,
                                        (GLsizei)range.numLayers,
                                        0, // border
                                        (GLsizei)numCompressedBytes, // TODO: does not work
                                                                     // for compressed
                                                                     // mipmaps
                                        data);
    } else {
      getContext().compressedTexSubImage3D(getTarget(),
                                           (GLint)range.mipLevel,
                                           (GLint)range.x,
                                           (GLint)range.y,
                                           (GLint)range.layer,
                                           (GLsizei)range.width,
                                           (GLsizei)range.height,
                                           (GLsizei)range.numLayers,
                                           formatDescGL_.internalFormat,
                                           (GLsizei)numCompressedBytes,
                                           data);
    }
  }
  return getContext().getLastError();
}

Result TextureBuffer::upload3D(GLenum target,
                               const TextureRangeDesc& range,
                               const void* data) const {
  const auto result = validateRange(range);
  if (!result.isOk()) {
    return result;
  }
  // Use TexImage when range covers full texture AND texture was not initialized with TexStorage
  const auto texImage = isValidForTexImage(range) && !supportsTexStorage();
  if (data == nullptr || !getProperties().isCompressed()) {
    if (texImage) {
      getContext().texImage3D(target,
                              (GLint)range.mipLevel,
                              formatDescGL_.internalFormat,
                              (GLsizei)range.width,
                              (GLsizei)range.height,
                              (GLsizei)range.depth,
                              0, // border
                              formatDescGL_.format,
                              formatDescGL_.type,
                              data);
    } else {
      getContext().texSubImage3D(target,
                                 (GLsizei)range.mipLevel,
                                 (GLsizei)range.x,
                                 (GLsizei)range.y,
                                 (GLsizei)range.z,
                                 (GLsizei)range.width,
                                 (GLsizei)range.height,
                                 (GLsizei)range.depth,
                                 formatDescGL_.format,
                                 formatDescGL_.type,
                                 data);
    }
  } else {
    const auto numCompressedBytes = getProperties().getBytesPerRange(range);
    IGL_ASSERT(numCompressedBytes > 0);
    if (texImage) {
      getContext().compressedTexImage3D(target,
                                        (GLint)range.mipLevel,
                                        formatDescGL_.internalFormat,
                                        (GLsizei)range.width,
                                        (GLsizei)range.height,
                                        (GLsizei)range.depth,
                                        0, // border
                                        (GLsizei)numCompressedBytes, // TODO: does not work
                                                                     // for compressed
                                                                     // mipmaps
                                        data);
    } else {
      getContext().compressedTexSubImage3D(getTarget(),
                                           (GLint)range.mipLevel,
                                           (GLint)range.x,
                                           (GLint)range.y,
                                           (GLint)range.z,
                                           (GLsizei)range.width,
                                           (GLsizei)range.height,
                                           (GLsizei)range.depth,
                                           formatDescGL_.internalFormat,
                                           (GLsizei)numCompressedBytes,
                                           data);
    }
  }
  return getContext().getLastError();
}

// upload data into the given mip level
// a sub-rect of the texture may be specified to only upload the sub-rect
Result TextureBuffer::upload(const TextureRangeDesc& range,
                             const void* data,
                             size_t bytesPerRow) const {
  if (data == nullptr) {
    return Result{};
  }
  const auto target = getTarget();
  if (target == 0) {
    return Result{Result::Code::InvalidOperation, "Unknown texture type"};
  }
  getContext().bindTexture(target, getId());

  auto result = upload(target, range, data, bytesPerRow);

  getContext().bindTexture(getTarget(), 0);
  return result;
}

Result TextureBuffer::upload(GLenum target,
                             const TextureRangeDesc& range,
                             const void* data,
                             size_t bytesPerRow) const {
  if (range.numMipLevels > 1) {
    IGL_ASSERT_NOT_IMPLEMENTED();
    return Result(Result::Code::Unimplemented,
                  "Uploading to more than 1 mip level is not yet supported.");
  }

  getContext().pixelStorei(GL_UNPACK_ALIGNMENT, this->getAlignment(bytesPerRow, range.mipLevel));

  Result success;
  switch (type_) {
  case TextureType::TwoD:
    return upload2D(target, range, data);
  case TextureType::TwoDArray:
    if (!getContext().deviceFeatures().hasFeature(DeviceFeatures::Texture2DArray)) {
      return Result(Result::Code::Unsupported, "Unsupported texture type");
    }
    return upload2DArray(target, range, data);
  case TextureType::ThreeD:
    if (!getContext().deviceFeatures().hasFeature(DeviceFeatures::Texture3D)) {
      return Result(Result::Code::Unsupported, "Unsupported texture type");
    }
    return upload3D(target, range, data);
  case TextureType::Cube: {
    for (auto cubeTarget : kCubeFaceTargets) {
      auto result = upload2D(cubeTarget, range, data);
      if (!result.isOk()) {
        return result;
      }
    }

    return Result{};
  }
  default:
    IGL_ASSERT_MSG(false, "Unknown texture type");
    return Result{Result::Code::InvalidOperation, "Unknown texture type"};
  }
}

Result TextureBuffer::uploadCube(const TextureRangeDesc& range,
                                 TextureCubeFace face,
                                 const void* data,
                                 size_t bytesPerRow) const {
  if (data == nullptr) {
    return Result{};
  }
  if (range.numMipLevels > 1) {
    IGL_ASSERT_NOT_IMPLEMENTED();
    return Result(Result::Code::Unimplemented,
                  "Uploading to more than 1 mip level is not yet supported.");
  }

  const auto target = getTarget();
  if (target != GL_TEXTURE_CUBE_MAP) {
    // this only uploads to cube textures
    return Result{Result::Code::InvalidOperation, "upload2D can only upload to 2D textures"};
  }

  getContext().pixelStorei(GL_UNPACK_ALIGNMENT, this->getAlignment(bytesPerRow, range.mipLevel));
  getContext().bindTexture(target, getId());

  IGL_ASSERT(range.numMipLevels == 1);

  GLenum cubeTarget = kCubeFaceTargets[static_cast<size_t>(face)];

  auto result = upload2D(cubeTarget, range, data);

  getContext().bindTexture(target, 0);

  return Result();
}

bool TextureBuffer::canInitialize() const {
  return !getProperties().isCompressed() ||
         (supportsTexStorage() && getContext().deviceFeatures().hasTextureFeature(
                                      TextureFeatures::TextureCompressionTexStorage)) ||
         getContext().deviceFeatures().hasTextureFeature(
             TextureFeatures::TextureCompressionTexImage);
}

bool TextureBuffer::supportsTexStorage() const {
  return (getUsage() & TextureDesc::TextureUsageBits::Storage) != 0 &&
         contains(getContext().deviceFeatures().getTextureFormatCapabilities(getFormat()),
                  ICapabilities::TextureFormatCapabilityBits::Storage);
}

} // namespace opengl
} // namespace igl
