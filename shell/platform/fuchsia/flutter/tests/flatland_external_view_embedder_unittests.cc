// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/fuchsia/flutter/flatland_external_view_embedder.h"

#include <fuchsia/math/cpp/fidl.h>
#include <fuchsia/sysmem/cpp/fidl.h>
#include <fuchsia/ui/composition/cpp/fidl.h>
#include <fuchsia/ui/views/cpp/fidl.h>
#include <lib/async-testing/test_loop.h>
#include <lib/zx/event.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "flutter/flow/embedded_views.h"
#include "flutter/fml/logging.h"
#include "flutter/fml/time/time_delta.h"
#include "flutter/fml/time/time_point.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkSize.h"
#include "third_party/skia/include/core/SkSurface.h"

#include "fakes/scenic/fake_flatland.h"
#include "fakes/scenic/fake_flatland_types.h"
#include "flutter/shell/platform/fuchsia/flutter/surface_producer.h"

#include "gmock/gmock.h"  // For EXPECT_THAT and matchers
#include "gtest/gtest.h"

using fuchsia::scenic::scheduling::FramePresentedInfo;
using fuchsia::scenic::scheduling::FuturePresentationTimes;
using fuchsia::scenic::scheduling::PresentReceivedInfo;
using ::testing::_;
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::FieldsAre;
using ::testing::IsEmpty;
using ::testing::Matcher;
using ::testing::Pointee;
using ::testing::Property;
using ::testing::SizeIs;
using ::testing::VariantWith;

namespace flutter_runner::testing {
namespace {

constexpr static fuchsia::ui::composition::BlendMode kFirstLayerBlendMode{
    fuchsia::ui::composition::BlendMode::SRC};
constexpr static fuchsia::ui::composition::BlendMode kUpperLayerBlendMode{
    fuchsia::ui::composition::BlendMode::SRC_OVER};

class FakeSurfaceProducerSurface : public SurfaceProducerSurface {
 public:
  explicit FakeSurfaceProducerSurface(
      fidl::InterfaceRequest<fuchsia::sysmem::BufferCollectionToken>
          sysmem_token_request,
      fuchsia::ui::composition::BufferCollectionImportToken buffer_import_token,
      const SkISize& size)
      : sysmem_token_request_(std::move(sysmem_token_request)),
        buffer_import_token_(std::move(buffer_import_token)),
        surface_(SkSurface::MakeNull(size.width(), size.height())) {
    zx_status_t acquire_status = zx::event::create(0, &acquire_fence_);
    if (acquire_status != ZX_OK) {
      FML_LOG(ERROR)
          << "FakeSurfaceProducerSurface: Failed to create acquire event";
    }
    zx_status_t release_status = zx::event::create(0, &release_fence_);
    if (release_status != ZX_OK) {
      FML_LOG(ERROR)
          << "FakeSurfaceProducerSurface: Failed to create release event";
    }
  }
  ~FakeSurfaceProducerSurface() override {}

  bool IsValid() const override { return true; }

  SkISize GetSize() const override {
    return SkISize::Make(surface_->width(), surface_->height());
  }

  void SetImageId(uint32_t image_id) override { image_id_ = image_id; }
  uint32_t GetImageId() override { return image_id_; }

  sk_sp<SkSurface> GetSkiaSurface() const override { return surface_; }

  fuchsia::ui::composition::BufferCollectionImportToken
  GetBufferCollectionImportToken() override {
    return std::move(buffer_import_token_);
  }

  zx::event GetAcquireFence() override {
    zx::event fence;
    acquire_fence_.duplicate(ZX_RIGHT_SAME_RIGHTS, &fence);
    return fence;
  }

  zx::event GetReleaseFence() override {
    zx::event fence;
    release_fence_.duplicate(ZX_RIGHT_SAME_RIGHTS, &fence);
    return fence;
  }

  void SetReleaseImageCallback(
      ReleaseImageCallback release_image_callback) override {}

  size_t AdvanceAndGetAge() override { return 0; }
  bool FlushSessionAcquireAndReleaseEvents() override { return true; }
  void SignalWritesFinished(
      const std::function<void(void)>& on_writes_committed) override {}

 private:
  fidl::InterfaceRequest<fuchsia::sysmem::BufferCollectionToken>
      sysmem_token_request_;
  fuchsia::ui::composition::BufferCollectionImportToken buffer_import_token_;
  zx::event acquire_fence_;
  zx::event release_fence_;

  sk_sp<SkSurface> surface_;
  uint32_t image_id_{0};
};

class FakeSurfaceProducer : public SurfaceProducer {
 public:
  explicit FakeSurfaceProducer(
      fuchsia::ui::composition::AllocatorHandle flatland_allocator)
      : flatland_allocator_(flatland_allocator.Bind()) {}
  ~FakeSurfaceProducer() override = default;

  // |SurfaceProducer|
  GrDirectContext* gr_context() const override { return nullptr; }

  // |SurfaceProducer|
  std::unique_ptr<SurfaceProducerSurface> ProduceOffscreenSurface(
      const SkISize& size) override {
    return nullptr;
  }

  // |SurfaceProducer|
  std::unique_ptr<SurfaceProducerSurface> ProduceSurface(
      const SkISize& size) override {
    auto [buffer_export_token, buffer_import_token] =
        BufferCollectionTokenPair::New();
    fuchsia::sysmem::BufferCollectionTokenHandle sysmem_token;
    auto sysmem_token_request = sysmem_token.NewRequest();

    fuchsia::ui::composition::RegisterBufferCollectionArgs
        buffer_collection_args;
    buffer_collection_args.set_export_token(std::move(buffer_export_token));
    buffer_collection_args.set_buffer_collection_token(std::move(sysmem_token));
    buffer_collection_args.set_usage(
        fuchsia::ui::composition::RegisterBufferCollectionUsage::DEFAULT);
    flatland_allocator_->RegisterBufferCollection(
        std::move(buffer_collection_args),
        [](fuchsia::ui::composition::Allocator_RegisterBufferCollection_Result
               result) {
          if (result.is_err()) {
            FAIL()
                << "fuhsia::ui::composition::RegisterBufferCollection error: "
                << static_cast<uint32_t>(result.err());
          }
        });

    return std::make_unique<FakeSurfaceProducerSurface>(
        std::move(sysmem_token_request), std::move(buffer_import_token), size);
  }

  // |SurfaceProducer|
  void SubmitSurfaces(
      std::vector<std::unique_ptr<SurfaceProducerSurface>> surfaces) override {}

  fuchsia::ui::composition::AllocatorPtr flatland_allocator_;
};

Matcher<fuchsia::ui::composition::ImageProperties> IsImageProperties(
    const fuchsia::math::SizeU& size) {
  return AllOf(
      Property("has_size", &fuchsia::ui::composition::ImageProperties::has_size,
               true),
      Property("size", &fuchsia::ui::composition::ImageProperties::size, size));
}

Matcher<fuchsia::ui::composition::ViewportProperties> IsViewportProperties(
    const fuchsia::math::SizeU& logical_size,
    const fuchsia::math::Inset& inset) {
  return AllOf(
      Property("has_logical_size",
               &fuchsia::ui::composition::ViewportProperties::has_logical_size,
               true),
      Property("logical_size",
               &fuchsia::ui::composition::ViewportProperties::logical_size,
               logical_size),
      Property("has_inset",
               &fuchsia::ui::composition::ViewportProperties::has_inset, true),
      Property("inset", &fuchsia::ui::composition::ViewportProperties::inset,
               inset));
}

Matcher<fuchsia::ui::composition::HitRegion> IsHitRegion(
    const float x,
    const float y,
    const float width,
    const float height,
    const fuchsia::ui::composition::HitTestInteraction hit_test) {
  return FieldsAre(FieldsAre(x, y, width, height), hit_test);
}

Matcher<FakeGraph> IsEmptyGraph() {
  return FieldsAre(IsEmpty(), IsEmpty(), Eq(nullptr), Eq(std::nullopt));
}

Matcher<FakeGraph> IsFlutterGraph(
    const fuchsia::ui::composition::ParentViewportWatcherPtr&
        parent_viewport_watcher,
    const fuchsia::ui::views::ViewportCreationToken& viewport_creation_token,
    const fuchsia::ui::views::ViewRef& view_ref,
    std::vector<Matcher<std::shared_ptr<FakeTransform>>> layer_matchers = {},
    fuchsia::math::VecF scale = FakeTransform::kDefaultScale) {
  auto viewport_token_koids = GetKoids(viewport_creation_token);
  auto view_ref_koids = GetKoids(view_ref);
  auto watcher_koids = GetKoids(parent_viewport_watcher);

  return FieldsAre(
      /*content_map*/ _, /*transform_map*/ _,
      Pointee(FieldsAre(
          /*id*/ _, FakeTransform::kDefaultTranslation,
          /*scale*/ scale, FakeTransform::kDefaultOrientation,
          /*clip_bounds*/ _, FakeTransform::kDefaultOpacity,
          /*children*/ ElementsAreArray(layer_matchers),
          /*content*/ Eq(nullptr), /*hit_regions*/ _)),
      Eq(FakeView{
          .view_token = viewport_token_koids.second,
          .view_ref = view_ref_koids.first,
          .view_ref_control = view_ref_koids.second,
          .view_ref_focused = ZX_KOID_INVALID,
          .focuser = ZX_KOID_INVALID,
          .touch_source = ZX_KOID_INVALID,
          .mouse_source = ZX_KOID_INVALID,
          .parent_viewport_watcher = watcher_koids.second,
      }));
}

Matcher<std::shared_ptr<FakeTransform>> IsImageLayer(
    const fuchsia::math::SizeU& layer_size,
    fuchsia::ui::composition::BlendMode blend_mode,
    std::vector<Matcher<fuchsia::ui::composition::HitRegion>>
        hit_region_matchers) {
  return Pointee(FieldsAre(
      /*id*/ _, FakeTransform::kDefaultTranslation,
      FakeTransform::kDefaultScale, FakeTransform::kDefaultOrientation,
      /*clip_bounds*/ _, FakeTransform::kDefaultOpacity,
      /*children*/ IsEmpty(),
      /*content*/
      Pointee(VariantWith<FakeImage>(FieldsAre(
          /*id*/ _, IsImageProperties(layer_size),
          FakeImage::kDefaultSampleRegion, layer_size,
          FakeImage::kDefaultOpacity, blend_mode,
          /*buffer_import_token*/ _, /*vmo_index*/ 0))),
      /* hit_regions*/ ElementsAreArray(hit_region_matchers)));
}

Matcher<std::shared_ptr<FakeTransform>> IsViewportLayer(
    const fuchsia::ui::views::ViewCreationToken& view_token,
    const fuchsia::math::SizeU& view_logical_size,
    const fuchsia::math::Inset& view_inset,
    const fuchsia::math::Vec& view_translation,
    const fuchsia::math::VecF& view_scale,
    const float view_opacity) {
  return Pointee(FieldsAre(
      /* id */ _, view_translation, view_scale,
      FakeTransform::kDefaultOrientation, /*clip_bounds*/ _, view_opacity,
      /*children*/ IsEmpty(),
      /*content*/
      Pointee(VariantWith<FakeViewport>(FieldsAre(
          /* id */ _, IsViewportProperties(view_logical_size, view_inset),
          /* viewport_token */ GetKoids(view_token).second,
          /* child_view_watcher */ _))),
      /*hit_regions*/ _));
}

fuchsia::ui::composition::OnNextFrameBeginValues WithPresentCredits(
    uint32_t additional_present_credits) {
  fuchsia::ui::composition::OnNextFrameBeginValues values;

  values.set_additional_present_credits(additional_present_credits);
  return values;
}

void DrawSimpleFrame(FlatlandExternalViewEmbedder& external_view_embedder,
                     SkISize frame_size,
                     float frame_dpr,
                     std::function<void(SkCanvas*)> draw_callback) {
  external_view_embedder.BeginFrame(frame_size, nullptr, frame_dpr, nullptr);
  {
    SkCanvas* root_canvas = external_view_embedder.GetRootCanvas();
    external_view_embedder.PostPrerollAction(nullptr);
    draw_callback(root_canvas);
  }
  external_view_embedder.EndFrame(false, nullptr);
  flutter::SurfaceFrame::FramebufferInfo framebuffer_info;
  framebuffer_info.supports_readback = true;
  external_view_embedder.SubmitFrame(
      nullptr, std::make_unique<flutter::SurfaceFrame>(
                   nullptr, std::move(framebuffer_info),
                   [](const flutter::SurfaceFrame& surface_frame,
                      SkCanvas* canvas) { return true; },
                   frame_size));
}

void DrawFrameWithView(FlatlandExternalViewEmbedder& external_view_embedder,
                       SkISize frame_size,
                       float frame_dpr,
                       int view_id,
                       flutter::EmbeddedViewParams& view_params,
                       std::function<void(SkCanvas*)> background_draw_callback,
                       std::function<void(SkCanvas*)> overlay_draw_callback) {
  external_view_embedder.BeginFrame(frame_size, nullptr, frame_dpr, nullptr);
  {
    SkCanvas* root_canvas = external_view_embedder.GetRootCanvas();
    external_view_embedder.PrerollCompositeEmbeddedView(
        view_id, std::make_unique<flutter::EmbeddedViewParams>(view_params));
    external_view_embedder.PostPrerollAction(nullptr);
    background_draw_callback(root_canvas);
    SkCanvas* overlay_canvas =
        external_view_embedder.CompositeEmbeddedView(view_id).canvas;
    overlay_draw_callback(overlay_canvas);
  }
  external_view_embedder.EndFrame(false, nullptr);
  flutter::SurfaceFrame::FramebufferInfo framebuffer_info;
  framebuffer_info.supports_readback = true;
  external_view_embedder.SubmitFrame(
      nullptr, std::make_unique<flutter::SurfaceFrame>(
                   nullptr, std::move(framebuffer_info),
                   [](const flutter::SurfaceFrame& surface_frame,
                      SkCanvas* canvas) { return true; },
                   frame_size));
}

};  // namespace

class FlatlandExternalViewEmbedderTest : public ::testing::Test {
 protected:
  FlatlandExternalViewEmbedderTest()
      : session_subloop_(loop_.StartNewLoop()),
        flatland_connection_(CreateFlatlandConnection()),
        fake_surface_producer_(
            std::make_shared<FakeSurfaceProducer>(CreateFlatlandAllocator())) {}
  ~FlatlandExternalViewEmbedderTest() override = default;

  async::TestLoop& loop() { return loop_; }

  std::shared_ptr<FakeSurfaceProducer> fake_surface_producer() {
    return fake_surface_producer_;
  }

  FakeFlatland& fake_flatland() { return fake_flatland_; }

  std::shared_ptr<FlatlandConnection> flatland_connection() {
    return flatland_connection_;
  }

 private:
  fuchsia::ui::composition::AllocatorHandle CreateFlatlandAllocator() {
    FML_CHECK(!fake_flatland_.is_allocator_connected());
    fuchsia::ui::composition::AllocatorHandle flatland_allocator =
        fake_flatland_.ConnectAllocator(session_subloop_->dispatcher());

    return flatland_allocator;
  }

  std::shared_ptr<FlatlandConnection> CreateFlatlandConnection() {
    FML_CHECK(!fake_flatland_.is_flatland_connected());
    fuchsia::ui::composition::FlatlandHandle flatland =
        fake_flatland_.ConnectFlatland(session_subloop_->dispatcher());

    auto test_name =
        ::testing::UnitTest::GetInstance()->current_test_info()->name();
    return std::make_shared<FlatlandConnection>(
        std::move(test_name), std::move(flatland), []() { FAIL(); },
        [](auto...) {}, 1, fml::TimeDelta::Zero());
  }

  // Primary loop and subloop for the FakeFlatland instance to process its
  // messages.  The subloop allocates it's own zx_port_t, allowing us to use a
  // separate port for each end of the message channel, rather than sharing a
  // single one.  Dual ports allow messages and responses to be intermingled,
  // which is how production code behaves; this improves test realism.
  async::TestLoop loop_;
  std::unique_ptr<async::LoopInterface> session_subloop_;

  FakeFlatland fake_flatland_;

  std::shared_ptr<FlatlandConnection> flatland_connection_;
  std::shared_ptr<FakeSurfaceProducer> fake_surface_producer_;
};

TEST_F(FlatlandExternalViewEmbedderTest, RootScene) {
  fuchsia::ui::composition::ParentViewportWatcherPtr parent_viewport_watcher;
  fuchsia::ui::views::ViewportCreationToken viewport_creation_token;
  fuchsia::ui::views::ViewCreationToken view_creation_token;
  fuchsia::ui::views::ViewRef view_ref;
  auto view_creation_token_status = zx::channel::create(
      0u, &viewport_creation_token.value, &view_creation_token.value);
  ASSERT_EQ(view_creation_token_status, ZX_OK);
  auto view_ref_pair = scenic::ViewRefPair::New();
  view_ref_pair.view_ref.Clone(&view_ref);

  FlatlandExternalViewEmbedder external_view_embedder(
      std::move(view_creation_token),
      fuchsia::ui::views::ViewIdentityOnCreation{
          .view_ref = std::move(view_ref_pair.view_ref),
          .view_ref_control = std::move(view_ref_pair.control_ref),
      },
      fuchsia::ui::composition::ViewBoundProtocols{},
      parent_viewport_watcher.NewRequest(), flatland_connection(),
      fake_surface_producer());
  EXPECT_THAT(fake_flatland().graph(), IsEmptyGraph());

  // Pump the loop; the graph should still be empty because nothing called
  // `Present` yet.
  loop().RunUntilIdle();
  EXPECT_THAT(fake_flatland().graph(), IsEmptyGraph());

  // Pump the loop; the contents of the initial `Present` should be processed.
  flatland_connection()->Present();
  loop().RunUntilIdle();
  EXPECT_THAT(fake_flatland().graph(),
              IsFlutterGraph(parent_viewport_watcher, viewport_creation_token,
                             view_ref));
}

TEST_F(FlatlandExternalViewEmbedderTest, SimpleScene) {
  fuchsia::ui::composition::ParentViewportWatcherPtr parent_viewport_watcher;
  fuchsia::ui::views::ViewportCreationToken viewport_creation_token;
  fuchsia::ui::views::ViewCreationToken view_creation_token;
  fuchsia::ui::views::ViewRef view_ref;
  auto view_creation_token_status = zx::channel::create(
      0u, &viewport_creation_token.value, &view_creation_token.value);
  ASSERT_EQ(view_creation_token_status, ZX_OK);
  auto view_ref_pair = scenic::ViewRefPair::New();
  view_ref_pair.view_ref.Clone(&view_ref);

  // Create the `FlatlandExternalViewEmbedder` and pump the message loop until
  // the initial scene graph is setup.
  FlatlandExternalViewEmbedder external_view_embedder(
      std::move(view_creation_token),
      fuchsia::ui::views::ViewIdentityOnCreation{
          .view_ref = std::move(view_ref_pair.view_ref),
          .view_ref_control = std::move(view_ref_pair.control_ref),
      },
      fuchsia::ui::composition::ViewBoundProtocols{},
      parent_viewport_watcher.NewRequest(), flatland_connection(),
      fake_surface_producer());
  flatland_connection()->Present();
  loop().RunUntilIdle();
  fake_flatland().FireOnNextFrameBeginEvent(WithPresentCredits(1u));
  loop().RunUntilIdle();
  EXPECT_THAT(fake_flatland().graph(),
              IsFlutterGraph(parent_viewport_watcher, viewport_creation_token,
                             view_ref));

  // Draw the scene.  The scene graph shouldn't change yet.
  const SkISize frame_size_signed = SkISize::Make(512, 512);
  const fuchsia::math::SizeU frame_size{
      static_cast<uint32_t>(frame_size_signed.width()),
      static_cast<uint32_t>(frame_size_signed.height())};
  DrawSimpleFrame(
      external_view_embedder, frame_size_signed, 1.f, [](SkCanvas* canvas) {
        const SkSize canvas_size = SkSize::Make(canvas->imageInfo().width(),
                                                canvas->imageInfo().height());
        SkPaint rect_paint;
        rect_paint.setColor(SK_ColorGREEN);
        canvas->translate(canvas_size.width() / 4.f,
                          canvas_size.height() / 2.f);
        canvas->drawRect(SkRect::MakeWH(canvas_size.width() / 32.f,
                                        canvas_size.height() / 32.f),
                         rect_paint);
      });
  EXPECT_THAT(fake_flatland().graph(),
              IsFlutterGraph(parent_viewport_watcher, viewport_creation_token,
                             view_ref));

  // Pump the message loop. The scene updates should propagate to flatland.
  loop().RunUntilIdle();

  EXPECT_THAT(
      fake_flatland().graph(),
      IsFlutterGraph(
          parent_viewport_watcher, viewport_creation_token, view_ref,
          /*layers*/
          {IsImageLayer(
              frame_size, kFirstLayerBlendMode,
              {IsHitRegion(
                  /* x */ 128.f,
                  /* y */ 256.f,
                  /* width */ 16.f,
                  /* height */ 16.f,
                  /* hit_test */
                  fuchsia::ui::composition::HitTestInteraction::DEFAULT)})}));
}

TEST_F(FlatlandExternalViewEmbedderTest, SceneWithOneView) {
  fuchsia::ui::composition::ParentViewportWatcherPtr parent_viewport_watcher;
  fuchsia::ui::views::ViewportCreationToken viewport_creation_token;
  fuchsia::ui::views::ViewCreationToken view_creation_token;
  fuchsia::ui::views::ViewRef view_ref;
  auto view_creation_token_status = zx::channel::create(
      0u, &viewport_creation_token.value, &view_creation_token.value);
  ASSERT_EQ(view_creation_token_status, ZX_OK);
  auto view_ref_pair = scenic::ViewRefPair::New();
  view_ref_pair.view_ref.Clone(&view_ref);

  // Create the `FlatlandExternalViewEmbedder` and pump the message loop until
  // the initial scene graph is setup.
  FlatlandExternalViewEmbedder external_view_embedder(
      std::move(view_creation_token),
      fuchsia::ui::views::ViewIdentityOnCreation{
          .view_ref = std::move(view_ref_pair.view_ref),
          .view_ref_control = std::move(view_ref_pair.control_ref),
      },
      fuchsia::ui::composition::ViewBoundProtocols{},
      parent_viewport_watcher.NewRequest(), flatland_connection(),
      fake_surface_producer());
  flatland_connection()->Present();
  loop().RunUntilIdle();
  fake_flatland().FireOnNextFrameBeginEvent(WithPresentCredits(1u));
  loop().RunUntilIdle();
  EXPECT_THAT(fake_flatland().graph(),
              IsFlutterGraph(parent_viewport_watcher, viewport_creation_token,
                             view_ref));

  // Create the view before drawing the scene.
  const SkSize child_view_size_signed = SkSize::Make(256.f, 512.f);
  const fuchsia::math::SizeU child_view_size{
      static_cast<uint32_t>(child_view_size_signed.width()),
      static_cast<uint32_t>(child_view_size_signed.height())};
  auto [child_view_token, child_viewport_token] = ViewTokenPair::New();
  const uint32_t child_view_id = child_viewport_token.value.get();

  const int kOpacity = 200;
  const float kOpacityFloat = 200 / 255.0f;
  const fuchsia::math::VecF kScale{3.0f, 4.0f};

  auto matrix = SkMatrix::I();
  matrix.setScaleX(kScale.x);
  matrix.setScaleY(kScale.y);

  auto mutators_stack = flutter::MutatorsStack();
  mutators_stack.PushOpacity(kOpacity);
  mutators_stack.PushTransform(matrix);

  flutter::EmbeddedViewParams child_view_params(matrix, child_view_size_signed,
                                                mutators_stack);
  external_view_embedder.CreateView(
      child_view_id, []() {},
      [](fuchsia::ui::composition::ContentId,
         fuchsia::ui::composition::ChildViewWatcherHandle) {});
  const SkRect child_view_occlusion_hint = SkRect::MakeLTRB(1, 2, 3, 4);
  const fuchsia::math::Inset child_view_inset{
      static_cast<int32_t>(child_view_occlusion_hint.top()),
      static_cast<int32_t>(child_view_occlusion_hint.right()),
      static_cast<int32_t>(child_view_occlusion_hint.bottom()),
      static_cast<int32_t>(child_view_occlusion_hint.left())};
  external_view_embedder.SetViewProperties(
      child_view_id, child_view_occlusion_hint, /*hit_testable=*/false,
      /*focusable=*/false);

  // We must take into account the effect of DPR on the view scale.
  const float kDPR = 2.0f;
  const float kInvDPR = 1.f / kDPR;

  // Draw the scene. The scene graph shouldn't change yet.
  const SkISize frame_size_signed = SkISize::Make(512, 512);
  const fuchsia::math::SizeU frame_size{
      static_cast<uint32_t>(frame_size_signed.width()),
      static_cast<uint32_t>(frame_size_signed.height())};
  DrawFrameWithView(
      external_view_embedder, frame_size_signed, kDPR, child_view_id,
      child_view_params,
      [](SkCanvas* canvas) {
        const SkSize canvas_size = SkSize::Make(canvas->imageInfo().width(),
                                                canvas->imageInfo().height());
        SkPaint rect_paint;
        rect_paint.setColor(SK_ColorGREEN);
        canvas->translate(canvas_size.width() / 4.f,
                          canvas_size.height() / 2.f);
        canvas->drawRect(SkRect::MakeWH(canvas_size.width() / 32.f,
                                        canvas_size.height() / 32.f),
                         rect_paint);
      },
      [](SkCanvas* canvas) {
        const SkSize canvas_size = SkSize::Make(canvas->imageInfo().width(),
                                                canvas->imageInfo().height());
        SkPaint rect_paint;
        rect_paint.setColor(SK_ColorRED);
        canvas->translate(canvas_size.width() * 3.f / 4.f,
                          canvas_size.height() / 2.f);
        canvas->drawRect(SkRect::MakeWH(canvas_size.width() / 32.f,
                                        canvas_size.height() / 32.f),
                         rect_paint);
      });
  EXPECT_THAT(fake_flatland().graph(),
              IsFlutterGraph(parent_viewport_watcher, viewport_creation_token,
                             view_ref));

  // Pump the message loop.  The scene updates should propagate to flatland.
  loop().RunUntilIdle();
  fake_flatland().FireOnNextFrameBeginEvent(WithPresentCredits(1u));
  loop().RunUntilIdle();

  EXPECT_THAT(
      fake_flatland().graph(),
      IsFlutterGraph(
          parent_viewport_watcher, viewport_creation_token, view_ref, /*layers*/
          {IsImageLayer(
               frame_size, kFirstLayerBlendMode,
               {IsHitRegion(
                   /* x */ 128.f,
                   /* y */ 256.f,
                   /* width */ 16.f,
                   /* height */ 16.f,
                   /* hit_test */
                   fuchsia::ui::composition::HitTestInteraction::DEFAULT)}),
           IsViewportLayer(child_view_token, child_view_size, child_view_inset,
                           {0, 0}, kScale, kOpacityFloat),
           IsImageLayer(
               frame_size, kUpperLayerBlendMode,
               {IsHitRegion(
                   /* x */ 384.f,
                   /* y */ 256.f,
                   /* width */ 16.f,
                   /* height */ 16.f,
                   /* hit_test */
                   fuchsia::ui::composition::HitTestInteraction::DEFAULT)})},
          {kInvDPR, kInvDPR}));

  // Destroy the view.  The scene graph shouldn't change yet.
  external_view_embedder.DestroyView(
      child_view_id, [](fuchsia::ui::composition::ContentId) {});
  EXPECT_THAT(
      fake_flatland().graph(),
      IsFlutterGraph(
          parent_viewport_watcher, viewport_creation_token, view_ref, /*layers*/
          {IsImageLayer(
               frame_size, kFirstLayerBlendMode,
               {IsHitRegion(
                   /* x */ 128.f,
                   /* y */ 256.f,
                   /* width */ 16.f,
                   /* height */ 16.f,
                   /* hit_test */
                   fuchsia::ui::composition::HitTestInteraction::DEFAULT)}),
           IsViewportLayer(child_view_token, child_view_size, child_view_inset,
                           {0, 0}, kScale, kOpacityFloat),
           IsImageLayer(
               frame_size, kUpperLayerBlendMode,
               {IsHitRegion(
                   /* x */ 384.f,
                   /* y */ 256.f,
                   /* width */ 16.f,
                   /* height */ 16.f,
                   /* hit_test */
                   fuchsia::ui::composition::HitTestInteraction::DEFAULT)})},
          {kInvDPR, kInvDPR}));

  // Draw another frame without the view.  The scene graph shouldn't change yet.
  DrawSimpleFrame(
      external_view_embedder, frame_size_signed, 1.f, [](SkCanvas* canvas) {
        const SkSize canvas_size = SkSize::Make(canvas->imageInfo().width(),
                                                canvas->imageInfo().height());
        SkPaint rect_paint;
        rect_paint.setColor(SK_ColorGREEN);
        canvas->translate(canvas_size.width() / 4.f,
                          canvas_size.height() / 2.f);
        canvas->drawRect(SkRect::MakeWH(canvas_size.width() / 32.f,
                                        canvas_size.height() / 32.f),
                         rect_paint);
      });
  EXPECT_THAT(
      fake_flatland().graph(),
      IsFlutterGraph(
          parent_viewport_watcher, viewport_creation_token, view_ref, /*layers*/
          {IsImageLayer(
               frame_size, kFirstLayerBlendMode,
               {IsHitRegion(
                   /* x */ 128.f,
                   /* y */ 256.f,
                   /* width */ 16.f,
                   /* height */ 16.f,
                   /* hit_test */
                   fuchsia::ui::composition::HitTestInteraction::DEFAULT)}),
           IsViewportLayer(child_view_token, child_view_size, child_view_inset,
                           {0, 0}, kScale, kOpacityFloat),
           IsImageLayer(
               frame_size, kUpperLayerBlendMode,
               {IsHitRegion(
                   /* x */ 384.f,
                   /* y */ 256.f,
                   /* width */ 16.f,
                   /* height */ 16.f,
                   /* hit_test */
                   fuchsia::ui::composition::HitTestInteraction::DEFAULT)})},
          {kInvDPR, kInvDPR}));

  // Pump the message loop.  The scene updates should propagate to flatland.
  loop().RunUntilIdle();
  EXPECT_THAT(
      fake_flatland().graph(),
      IsFlutterGraph(
          parent_viewport_watcher, viewport_creation_token, view_ref, /*layers*/
          {IsImageLayer(
              frame_size, kFirstLayerBlendMode,
              {IsHitRegion(
                  /* x */ 128.f,
                  /* y */ 256.f,
                  /* width */ 16.f,
                  /* height */ 16.f,
                  /* hit_test */
                  fuchsia::ui::composition::HitTestInteraction::DEFAULT)})}));
}

TEST_F(FlatlandExternalViewEmbedderTest, SceneWithOneView_NoOverlay) {
  fuchsia::ui::composition::ParentViewportWatcherPtr parent_viewport_watcher;
  fuchsia::ui::views::ViewportCreationToken viewport_creation_token;
  fuchsia::ui::views::ViewCreationToken view_creation_token;
  fuchsia::ui::views::ViewRef view_ref;
  auto view_creation_token_status = zx::channel::create(
      0u, &viewport_creation_token.value, &view_creation_token.value);
  ASSERT_EQ(view_creation_token_status, ZX_OK);
  auto view_ref_pair = scenic::ViewRefPair::New();
  view_ref_pair.view_ref.Clone(&view_ref);

  // Create the `FlatlandExternalViewEmbedder` and pump the message loop until
  // the initial scene graph is setup.
  FlatlandExternalViewEmbedder external_view_embedder(
      std::move(view_creation_token),
      fuchsia::ui::views::ViewIdentityOnCreation{
          .view_ref = std::move(view_ref_pair.view_ref),
          .view_ref_control = std::move(view_ref_pair.control_ref),
      },
      fuchsia::ui::composition::ViewBoundProtocols{},
      parent_viewport_watcher.NewRequest(), flatland_connection(),
      fake_surface_producer());
  flatland_connection()->Present();
  loop().RunUntilIdle();
  fake_flatland().FireOnNextFrameBeginEvent(WithPresentCredits(1u));
  loop().RunUntilIdle();
  EXPECT_THAT(fake_flatland().graph(),
              IsFlutterGraph(parent_viewport_watcher, viewport_creation_token,
                             view_ref));

  // Create the view before drawing the scene.
  const SkSize child_view_size_signed = SkSize::Make(256.f, 512.f);
  const fuchsia::math::SizeU child_view_size{
      static_cast<uint32_t>(child_view_size_signed.width()),
      static_cast<uint32_t>(child_view_size_signed.height())};
  auto [child_view_token, child_viewport_token] = ViewTokenPair::New();
  const uint32_t child_view_id = child_viewport_token.value.get();

  const int kOpacity = 125;
  const float kOpacityFloat = 125 / 255.0f;
  const fuchsia::math::VecF kScale{2.f, 3.0f};

  auto matrix = SkMatrix::I();
  matrix.setScaleX(kScale.x);
  matrix.setScaleY(kScale.y);

  auto mutators_stack = flutter::MutatorsStack();
  mutators_stack.PushOpacity(kOpacity);
  mutators_stack.PushTransform(matrix);

  flutter::EmbeddedViewParams child_view_params(matrix, child_view_size_signed,
                                                mutators_stack);
  external_view_embedder.CreateView(
      child_view_id, []() {},
      [](fuchsia::ui::composition::ContentId,
         fuchsia::ui::composition::ChildViewWatcherHandle) {});

  // Draw the scene.  The scene graph shouldn't change yet.
  const SkISize frame_size_signed = SkISize::Make(512, 512);
  const fuchsia::math::SizeU frame_size{
      static_cast<uint32_t>(frame_size_signed.width()),
      static_cast<uint32_t>(frame_size_signed.height())};
  DrawFrameWithView(
      external_view_embedder, frame_size_signed, 1.f, child_view_id,
      child_view_params,
      [](SkCanvas* canvas) {
        const SkSize canvas_size = SkSize::Make(canvas->imageInfo().width(),
                                                canvas->imageInfo().height());
        SkPaint rect_paint;
        rect_paint.setColor(SK_ColorGREEN);
        canvas->translate(canvas_size.width() / 4.f,
                          canvas_size.height() / 2.f);
        canvas->drawRect(SkRect::MakeWH(canvas_size.width() / 32.f,
                                        canvas_size.height() / 32.f),
                         rect_paint);
      },
      [](SkCanvas* canvas) {});
  EXPECT_THAT(fake_flatland().graph(),
              IsFlutterGraph(parent_viewport_watcher, viewport_creation_token,
                             view_ref));

  // Pump the message loop.  The scene updates should propagate to flatland.
  loop().RunUntilIdle();
  fake_flatland().FireOnNextFrameBeginEvent(WithPresentCredits(1u));
  loop().RunUntilIdle();
  EXPECT_THAT(
      fake_flatland().graph(),
      IsFlutterGraph(
          parent_viewport_watcher, viewport_creation_token, view_ref, /*layers*/
          {IsImageLayer(
               frame_size, kFirstLayerBlendMode,
               {IsHitRegion(
                   /* x */ 128.f,
                   /* y */ 256.f,
                   /* width */ 16.f,
                   /* height */ 16.f,
                   /* hit_test */
                   fuchsia::ui::composition::HitTestInteraction::DEFAULT)}),
           IsViewportLayer(child_view_token, child_view_size,
                           FakeViewport::kDefaultViewportInset, {0, 0}, kScale,
                           kOpacityFloat)}));

  // Destroy the view.  The scene graph shouldn't change yet.
  external_view_embedder.DestroyView(
      child_view_id, [](fuchsia::ui::composition::ContentId) {});
  EXPECT_THAT(
      fake_flatland().graph(),
      IsFlutterGraph(
          parent_viewport_watcher, viewport_creation_token, view_ref, /*layers*/
          {IsImageLayer(
               frame_size, kFirstLayerBlendMode,
               {IsHitRegion(
                   /* x */ 128.f,
                   /* y */ 256.f,
                   /* width */ 16.f,
                   /* height */ 16.f,
                   /* hit_test */
                   fuchsia::ui::composition::HitTestInteraction::DEFAULT)}),
           IsViewportLayer(child_view_token, child_view_size,
                           FakeViewport::kDefaultViewportInset, {0, 0}, kScale,
                           kOpacityFloat)}));

  // Draw another frame without the view.  The scene graph shouldn't change yet.
  DrawSimpleFrame(
      external_view_embedder, frame_size_signed, 1.f, [](SkCanvas* canvas) {
        const SkSize canvas_size = SkSize::Make(canvas->imageInfo().width(),
                                                canvas->imageInfo().height());
        SkPaint rect_paint;
        rect_paint.setColor(SK_ColorGREEN);
        canvas->translate(canvas_size.width() / 4.f,
                          canvas_size.height() / 2.f);
        canvas->drawRect(SkRect::MakeWH(canvas_size.width() / 32.f,
                                        canvas_size.height() / 32.f),
                         rect_paint);
      });

  EXPECT_THAT(
      fake_flatland().graph(),
      IsFlutterGraph(
          parent_viewport_watcher, viewport_creation_token, view_ref, /*layers*/
          {IsImageLayer(
               frame_size, kFirstLayerBlendMode,
               {IsHitRegion(
                   /* x */ 128.f,
                   /* y */ 256.f,
                   /* width */ 16.f,
                   /* height */ 16.f,
                   /* hit_test */
                   fuchsia::ui::composition::HitTestInteraction::DEFAULT)}),
           IsViewportLayer(child_view_token, child_view_size,
                           FakeViewport::kDefaultViewportInset, {0, 0}, kScale,
                           kOpacityFloat)}));

  // Pump the message loop.  The scene updates should propagate to flatland.
  loop().RunUntilIdle();
  EXPECT_THAT(
      fake_flatland().graph(),
      IsFlutterGraph(
          parent_viewport_watcher, viewport_creation_token, view_ref, /*layers*/
          {IsImageLayer(
              frame_size, kFirstLayerBlendMode,
              {IsHitRegion(
                  /* x */ 128.f,
                  /* y */ 256.f,
                  /* width */ 16.f,
                  /* height */ 16.f,
                  /* hit_test */
                  fuchsia::ui::composition::HitTestInteraction::DEFAULT)})}));
}

TEST_F(FlatlandExternalViewEmbedderTest,
       SceneWithOneView_DestroyBeforeDrawing) {
  fuchsia::ui::composition::ParentViewportWatcherPtr parent_viewport_watcher;
  fuchsia::ui::views::ViewportCreationToken viewport_creation_token;
  fuchsia::ui::views::ViewCreationToken view_creation_token;
  fuchsia::ui::views::ViewRef view_ref;
  auto view_creation_token_status = zx::channel::create(
      0u, &viewport_creation_token.value, &view_creation_token.value);
  ASSERT_EQ(view_creation_token_status, ZX_OK);
  auto view_ref_pair = scenic::ViewRefPair::New();
  view_ref_pair.view_ref.Clone(&view_ref);

  // Create the `FlatlandExternalViewEmbedder` and pump the message loop until
  // the initial scene graph is setup.
  FlatlandExternalViewEmbedder external_view_embedder(
      std::move(view_creation_token),
      fuchsia::ui::views::ViewIdentityOnCreation{
          .view_ref = std::move(view_ref_pair.view_ref),
          .view_ref_control = std::move(view_ref_pair.control_ref),
      },
      fuchsia::ui::composition::ViewBoundProtocols{},
      parent_viewport_watcher.NewRequest(), flatland_connection(),
      fake_surface_producer());
  flatland_connection()->Present();
  loop().RunUntilIdle();
  fake_flatland().FireOnNextFrameBeginEvent(WithPresentCredits(1u));
  loop().RunUntilIdle();
  EXPECT_THAT(fake_flatland().graph(),
              IsFlutterGraph(parent_viewport_watcher, viewport_creation_token,
                             view_ref));

  // Create the view before drawing the scene.
  auto [child_view_token, child_viewport_token] = ViewTokenPair::New();
  const uint32_t child_view_id = child_viewport_token.value.get();
  external_view_embedder.CreateView(
      child_view_id, []() {},
      [](fuchsia::ui::composition::ContentId,
         fuchsia::ui::composition::ChildViewWatcherHandle) {});

  // Draw the scene without the view. The scene graph shouldn't change yet.
  const SkISize frame_size_signed = SkISize::Make(512, 512);
  const fuchsia::math::SizeU frame_size{
      static_cast<uint32_t>(frame_size_signed.width()),
      static_cast<uint32_t>(frame_size_signed.height())};
  DrawSimpleFrame(
      external_view_embedder, frame_size_signed, 1.f, [](SkCanvas* canvas) {
        const SkSize canvas_size = SkSize::Make(canvas->imageInfo().width(),
                                                canvas->imageInfo().height());
        SkPaint rect_paint;
        rect_paint.setColor(SK_ColorGREEN);
        canvas->translate(canvas_size.width() / 4.f,
                          canvas_size.height() / 2.f);
        canvas->drawRect(SkRect::MakeWH(canvas_size.width() / 32.f,
                                        canvas_size.height() / 32.f),
                         rect_paint);
      });

  // Pump the message loop. The scene updates should propagate to flatland.
  loop().RunUntilIdle();
  fake_flatland().FireOnNextFrameBeginEvent(WithPresentCredits(1u));
  loop().RunUntilIdle();
  EXPECT_THAT(
      fake_flatland().graph(),
      IsFlutterGraph(
          parent_viewport_watcher, viewport_creation_token, view_ref,
          /*layers*/
          {IsImageLayer(
              frame_size, kFirstLayerBlendMode,
              {IsHitRegion(
                  /* x */ 128.f,
                  /* y */ 256.f,
                  /* width */ 16.f,
                  /* height */ 16.f,
                  /* hit_test */
                  fuchsia::ui::composition::HitTestInteraction::DEFAULT)})}));

  // Destroy the view.  The scene graph shouldn't change yet.
  external_view_embedder.DestroyView(
      child_view_id, [](fuchsia::ui::composition::ContentId) {});
  EXPECT_THAT(
      fake_flatland().graph(),
      IsFlutterGraph(
          parent_viewport_watcher, viewport_creation_token, view_ref,
          /*layers*/
          {IsImageLayer(
              frame_size, kFirstLayerBlendMode,
              {IsHitRegion(
                  /* x */ 128.f,
                  /* y */ 256.f,
                  /* width */ 16.f,
                  /* height */ 16.f,
                  /* hit_test */
                  fuchsia::ui::composition::HitTestInteraction::DEFAULT)})}));

  // Draw another frame without the view and change the size. The scene graph
  // shouldn't change yet.
  const SkISize new_frame_size_signed = SkISize::Make(256, 256);
  const fuchsia::math::SizeU new_frame_size{
      static_cast<uint32_t>(new_frame_size_signed.width()),
      static_cast<uint32_t>(new_frame_size_signed.height())};
  DrawSimpleFrame(
      external_view_embedder, new_frame_size_signed, 1.f, [](SkCanvas* canvas) {
        const SkSize canvas_size = SkSize::Make(canvas->imageInfo().width(),
                                                canvas->imageInfo().height());
        SkPaint rect_paint;
        rect_paint.setColor(SK_ColorGREEN);
        canvas->translate(canvas_size.width() / 4.f,
                          canvas_size.height() / 2.f);
        canvas->drawRect(SkRect::MakeWH(canvas_size.width() / 32.f,
                                        canvas_size.height() / 32.f),
                         rect_paint);
      });
  EXPECT_THAT(
      fake_flatland().graph(),
      IsFlutterGraph(
          parent_viewport_watcher, viewport_creation_token, view_ref,
          /*layers*/
          {IsImageLayer(
              frame_size, kFirstLayerBlendMode,
              {IsHitRegion(
                  /* x */ 128.f,
                  /* y */ 256.f,
                  /* width */ 16.f,
                  /* height */ 16.f,
                  /* hit_test */
                  fuchsia::ui::composition::HitTestInteraction::DEFAULT)})}));

  // Pump the message loop.  The scene updates should propagate to flatland.
  loop().RunUntilIdle();
  EXPECT_THAT(
      fake_flatland().graph(),
      IsFlutterGraph(
          parent_viewport_watcher, viewport_creation_token, view_ref, /*layers*/
          {IsImageLayer(
              new_frame_size, kFirstLayerBlendMode,
              {IsHitRegion(
                  /* x */ 64.f,
                  /* y */ 128.f,
                  /* width */ 8.f,
                  /* height */ 8.f,
                  /* hit_test */
                  fuchsia::ui::composition::HitTestInteraction::DEFAULT)})}));
}

// This test case exercises the scenario in which the view contains two disjoint
// regions with painted content; we should generate two separate hit regions
// matching the bounds of the painted regions in this case.
TEST_F(FlatlandExternalViewEmbedderTest, SimpleScene_DisjointHitRegions) {
  fuchsia::ui::composition::ParentViewportWatcherPtr parent_viewport_watcher;
  fuchsia::ui::views::ViewportCreationToken viewport_creation_token;
  fuchsia::ui::views::ViewCreationToken view_creation_token;
  fuchsia::ui::views::ViewRef view_ref;
  auto view_creation_token_status = zx::channel::create(
      0u, &viewport_creation_token.value, &view_creation_token.value);
  ASSERT_EQ(view_creation_token_status, ZX_OK);
  auto view_ref_pair = scenic::ViewRefPair::New();
  view_ref_pair.view_ref.Clone(&view_ref);

  // Create the `FlatlandExternalViewEmbedder` and pump the message loop until
  // the initial scene graph is setup.
  FlatlandExternalViewEmbedder external_view_embedder(
      std::move(view_creation_token),
      fuchsia::ui::views::ViewIdentityOnCreation{
          .view_ref = std::move(view_ref_pair.view_ref),
          .view_ref_control = std::move(view_ref_pair.control_ref),
      },
      fuchsia::ui::composition::ViewBoundProtocols{},
      parent_viewport_watcher.NewRequest(), flatland_connection(),
      fake_surface_producer());
  flatland_connection()->Present();
  loop().RunUntilIdle();
  fake_flatland().FireOnNextFrameBeginEvent(WithPresentCredits(1u));
  loop().RunUntilIdle();
  EXPECT_THAT(fake_flatland().graph(),
              IsFlutterGraph(parent_viewport_watcher, viewport_creation_token,
                             view_ref));

  // Draw the scene.  The scene graph shouldn't change yet.
  const SkISize frame_size_signed = SkISize::Make(512, 512);
  const fuchsia::math::SizeU frame_size{
      static_cast<uint32_t>(frame_size_signed.width()),
      static_cast<uint32_t>(frame_size_signed.height())};
  DrawSimpleFrame(
      external_view_embedder, frame_size_signed, 1.f, [](SkCanvas* canvas) {
        const SkSize canvas_size = SkSize::Make(canvas->imageInfo().width(),
                                                canvas->imageInfo().height());

        SkRect paint_region_1, paint_region_2;

        paint_region_1 = SkRect::MakeXYWH(
            canvas_size.width() / 4.f, canvas_size.height() / 2.f,
            canvas_size.width() / 32.f, canvas_size.height() / 32.f);

        SkPaint rect_paint;
        rect_paint.setColor(SK_ColorGREEN);
        canvas->drawRect(paint_region_1, rect_paint);

        paint_region_2 = SkRect::MakeXYWH(
            canvas_size.width() * 3.f / 4.f, canvas_size.height() / 2.f,
            canvas_size.width() / 32.f, canvas_size.height() / 32.f);

        rect_paint.setColor(SK_ColorRED);
        canvas->drawRect(paint_region_2, rect_paint);
      });
  EXPECT_THAT(fake_flatland().graph(),
              IsFlutterGraph(parent_viewport_watcher, viewport_creation_token,
                             view_ref));

  // Pump the message loop. The scene updates should propagate to flatland.
  loop().RunUntilIdle();

  EXPECT_THAT(
      fake_flatland().graph(),
      IsFlutterGraph(
          parent_viewport_watcher, viewport_creation_token, view_ref,
          /*layers*/
          {IsImageLayer(
              frame_size, kFirstLayerBlendMode,
              {IsHitRegion(
                   /* x */ 128.f,
                   /* y */ 256.f,
                   /* width */ 16.f,
                   /* height */ 16.f,
                   /* hit_test */
                   fuchsia::ui::composition::HitTestInteraction::DEFAULT),
               IsHitRegion(
                   /* x */ 384.f,
                   /* y */ 256.f,
                   /* width */ 16.f,
                   /* height */ 16.f,
                   /* hit_test */
                   fuchsia::ui::composition::HitTestInteraction::DEFAULT)})}));
}

// This test case exercises the scenario in which the view contains two
// overlapping regions with painted content; we should generate one hit
// region matching the union of the bounds of the two painted regions in
// this case.
TEST_F(FlatlandExternalViewEmbedderTest, SimpleScene_OverlappingHitRegions) {
  fuchsia::ui::composition::ParentViewportWatcherPtr parent_viewport_watcher;
  fuchsia::ui::views::ViewportCreationToken viewport_creation_token;
  fuchsia::ui::views::ViewCreationToken view_creation_token;
  fuchsia::ui::views::ViewRef view_ref;
  auto view_creation_token_status = zx::channel::create(
      0u, &viewport_creation_token.value, &view_creation_token.value);
  ASSERT_EQ(view_creation_token_status, ZX_OK);
  auto view_ref_pair = scenic::ViewRefPair::New();
  view_ref_pair.view_ref.Clone(&view_ref);

  // Create the `FlatlandExternalViewEmbedder` and pump the message loop until
  // the initial scene graph is setup.
  FlatlandExternalViewEmbedder external_view_embedder(
      std::move(view_creation_token),
      fuchsia::ui::views::ViewIdentityOnCreation{
          .view_ref = std::move(view_ref_pair.view_ref),
          .view_ref_control = std::move(view_ref_pair.control_ref),
      },
      fuchsia::ui::composition::ViewBoundProtocols{},
      parent_viewport_watcher.NewRequest(), flatland_connection(),
      fake_surface_producer());
  flatland_connection()->Present();
  loop().RunUntilIdle();
  fake_flatland().FireOnNextFrameBeginEvent(WithPresentCredits(1u));
  loop().RunUntilIdle();
  EXPECT_THAT(fake_flatland().graph(),
              IsFlutterGraph(parent_viewport_watcher, viewport_creation_token,
                             view_ref));

  // Draw the scene.  The scene graph shouldn't change yet.
  const SkISize frame_size_signed = SkISize::Make(512, 512);
  const fuchsia::math::SizeU frame_size{
      static_cast<uint32_t>(frame_size_signed.width()),
      static_cast<uint32_t>(frame_size_signed.height())};
  DrawSimpleFrame(
      external_view_embedder, frame_size_signed, 1.f, [](SkCanvas* canvas) {
        const SkSize canvas_size = SkSize::Make(canvas->imageInfo().width(),
                                                canvas->imageInfo().height());

        SkRect paint_region_1, paint_region_2;

        paint_region_1 = SkRect::MakeXYWH(
            canvas_size.width() / 4.f, canvas_size.height() / 2.f,
            3.f * canvas_size.width() / 8.f, canvas_size.height() / 4.f);

        SkPaint rect_paint;
        rect_paint.setColor(SK_ColorGREEN);
        canvas->drawRect(paint_region_1, rect_paint);

        paint_region_2 = SkRect::MakeXYWH(
            canvas_size.width() * 3.f / 8.f, canvas_size.height() / 2.f,
            3.f * canvas_size.width() / 8.f, canvas_size.height() / 4.f);

        rect_paint.setColor(SK_ColorRED);
        canvas->drawRect(paint_region_2, rect_paint);
      });
  EXPECT_THAT(fake_flatland().graph(),
              IsFlutterGraph(parent_viewport_watcher, viewport_creation_token,
                             view_ref));

  // Pump the message loop. The scene updates should propagate to flatland.
  loop().RunUntilIdle();

  EXPECT_THAT(
      fake_flatland().graph(),
      IsFlutterGraph(
          parent_viewport_watcher, viewport_creation_token, view_ref,
          /*layers*/
          {IsImageLayer(
              frame_size, kFirstLayerBlendMode,
              {IsHitRegion(
                  /* x */ 128.f,
                  /* y */ 256.f,
                  /* width */ 256.f,
                  /* height */ 128.f,
                  /* hit_test */
                  fuchsia::ui::composition::HitTestInteraction::DEFAULT)})}));
}

}  // namespace flutter_runner::testing
