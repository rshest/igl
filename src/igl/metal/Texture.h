/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <igl/Macros.h>
#include <igl/Texture.h>
#include <igl/metal/CommandQueue.h>

#if IGL_PLATFORM_APPLE
NS_ASSUME_NONNULL_BEGIN
#endif

namespace igl {
namespace metal {
class PlatformDevice;

class Texture final : public ITexture {
  friend class Device;
  friend class PlatformDevice;

 public:
  Texture(id<MTLTexture> texture, const ICapabilities& capabilities);
  Texture(id<CAMetalDrawable> drawable, const ICapabilities& capabilities);
  ~Texture() override;

  Result upload(const TextureRangeDesc& range,
                const void* data,
                size_t bytesPerRow = 0) const override;
  Result uploadCube(const TextureRangeDesc& range,
                    TextureCubeFace face,
                    const void* data,
                    size_t bytesPerRow = 0) const override;

  Result getBytes(const TextureRangeDesc& range, void* outData, size_t bytesPerRow = 0) const;

  // Accessors
  Dimensions getDimensions() const override;
  size_t getNumLayers() const override;
  TextureType getType() const override;
  TextureDesc::TextureUsage getUsage() const override;
  uint32_t getSamples() const override;
  uint32_t getNumMipLevels() const override;
  void generateMipmap(ICommandQueue& cmdQueue) const override;
  void generateMipmap(ICommandBuffer& cmdBuffer) const override;
  bool isRequiredGenerateMipmap() const override;
  uint64_t getTextureId() const override;

  IGL_INLINE id<MTLTexture> _Nullable get() const {
    return (drawable_) ? drawable_.texture : value_;
  }
  IGL_INLINE id<CAMetalDrawable> _Nullable getDrawable() const {
    return drawable_;
  }

  static TextureDesc::TextureUsage toTextureUsage(MTLTextureUsage usage);
  static MTLTextureUsage toMTLTextureUsage(TextureDesc::TextureUsage usage);

  static MTLPixelFormat textureFormatToMTLPixelFormat(TextureFormat value);
  static TextureFormat mtlPixelFormatToTextureFormat(MTLPixelFormat value);
  static MTLTextureType convertType(TextureType value, size_t numSamples);
  static TextureType convertType(MTLTextureType value);

 private:
  void generateMipmap(id<MTLCommandBuffer> cmdBuffer) const;

  // Given bytes per row of an input texture, return bytesPerRow value
  // accepted by Texture::upload and MTL replaceRegion.
  size_t toMetalBytesPerRow(size_t bytesPerRow) const;

  id<MTLTexture> _Nullable value_;
  id<CAMetalDrawable> _Nullable drawable_;
  const ICapabilities& capabilities_;
};

} // namespace metal
} // namespace igl

#if IGL_PLATFORM_APPLE
NS_ASSUME_NONNULL_END
#endif
