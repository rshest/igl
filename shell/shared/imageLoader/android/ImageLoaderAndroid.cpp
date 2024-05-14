/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <shell/shared/imageLoader/android/ImageLoaderAndroid.h>

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>

namespace igl::shell {

ImageLoaderAndroid::ImageLoaderAndroid(FileLoader& fileLoader) : ImageLoader(fileLoader) {}

ImageData ImageLoaderAndroid::loadImageData(const std::string& imageName) noexcept {
  if (imageName.empty()) {
    IGL_LOG_ERROR("Error in loadBinaryData(): empty fileName\n");
    return {};
  }

  if (assetManager_ == nullptr) {
    IGL_LOG_INFO("Asset manager not set!\n");
    // Fallback to default behavior (i.e., loading w/ C++ functions) when asset manager is not set
    // as this is the case for some unit tests.
    return ImageLoader::loadImageData(imageName);
  }

  // Load file
  AAsset* asset = AAssetManager_open(assetManager_, imageName.c_str(), AASSET_MODE_BUFFER);
  if (IGL_UNEXPECTED(asset == nullptr)) {
    IGL_LOG_ERROR("Error in loadImageData(): failed to open file %s\n", imageName.c_str());
    return {};
  }

  off64_t length = AAsset_getLength64(asset);
  if (IGL_UNEXPECTED(length > std::numeric_limits<int>::max())) {
    AAsset_close(asset);
    return {};
  }

  auto buffer = std::make_unique<uint8_t[]>(length);
  auto readSize = AAsset_read(asset, buffer.get(), length);
  if (IGL_UNEXPECTED(readSize != length)) {
    IGL_LOG_ERROR("Error in loadImageData(): read size mismatch (%ld != %zu) in %s\n",
                  readSize,
                  length,
                  imageName.c_str());
  }
  AAsset_close(asset);

  return loadImageDataFromMemory(buffer.get(), length);
}

} // namespace igl::shell
