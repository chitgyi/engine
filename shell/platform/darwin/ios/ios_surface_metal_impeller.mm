// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "flutter/shell/platform/darwin/ios/ios_surface_metal_impeller.h"

#include "flutter/shell/gpu/gpu_surface_metal_impeller.h"

namespace flutter {

IOSSurfaceMetalImpeller::IOSSurfaceMetalImpeller(const fml::scoped_nsobject<CAMetalLayer>& layer,
                                                 const std::shared_ptr<IOSContext>& context)
    : IOSSurface(context),
      GPUSurfaceMetalDelegate(MTLRenderTargetType::kCAMetalLayer),
      layer_(layer),
      impeller_context_(context ? context->GetImpellerContext() : nullptr) {
  if (!impeller_context_) {
    return;
  }
  is_valid_ = true;
}

// |IOSSurface|
IOSSurfaceMetalImpeller::~IOSSurfaceMetalImpeller() = default;

// |IOSSurface|
bool IOSSurfaceMetalImpeller::IsValid() const {
  return is_valid_;
}

// |IOSSurface|
void IOSSurfaceMetalImpeller::UpdateStorageSizeIfNecessary() {
  // Nothing to do.
}

// |IOSSurface|
std::unique_ptr<Surface> IOSSurfaceMetalImpeller::CreateGPUSurface(GrDirectContext*) {
  return std::make_unique<GPUSurfaceMetalImpeller>(this,              //
                                                   impeller_context_  //

  );
}

// |GPUSurfaceMetalDelegate|
GPUCAMetalLayerHandle IOSSurfaceMetalImpeller::GetCAMetalLayer(const SkISize& frame_info) const {
  CAMetalLayer* layer = layer_.get();
  const auto drawable_size = CGSizeMake(frame_info.width(), frame_info.height());
  if (!CGSizeEqualToSize(drawable_size, layer.drawableSize)) {
    layer.drawableSize = drawable_size;
  }
  return layer;
}

// |GPUSurfaceMetalDelegate|
bool IOSSurfaceMetalImpeller::PresentDrawable(GrMTLHandle drawable) const {
  FML_DCHECK(false);
  return false;
}

// |GPUSurfaceMetalDelegate|
GPUMTLTextureInfo IOSSurfaceMetalImpeller::GetMTLTexture(const SkISize& frame_info) const {
  FML_CHECK(false);
  return GPUMTLTextureInfo{
      .texture_id = -1,   //
      .texture = nullptr  //
  };
}

// |GPUSurfaceMetalDelegate|
bool IOSSurfaceMetalImpeller::PresentTexture(GPUMTLTextureInfo texture) const {
  FML_CHECK(false);
  return false;
}

// |GPUSurfaceMetalDelegate|
bool IOSSurfaceMetalImpeller::AllowsDrawingWhenGpuDisabled() const {
  return false;
}

}  // namespace flutter
