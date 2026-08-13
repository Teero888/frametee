//! Bevy's renderer, drawing this game's frame.
//!
//! The module owns a Bevy `App` with the real rendering stack — `Camera2d`,
//! sprites, the lot — running on the engine's Vulkan device. Its camera targets
//! an image the engine allocated, so Bevy draws into engine memory and the
//! engine composites the result into its viewport. No pixels are copied.
//!
//! Two things about the setup are load-bearing rather than incidental:
//!
//!  * `PipelinedRenderingPlugin` is disabled. It moves rendering to another
//!    thread, and this design depends on Bevy having submitted its work to the
//!    shared queue by the time the render callback returns — that ordering is
//!    what makes the engine's barriers sufficient.
//!  * `WindowPlugin` gets no primary window. The module has no window; the
//!    engine owns it, and Bevy never needs to know it exists.

use std::os::raw::c_void;
use std::sync::Arc;

use bevy::app::App;
use bevy::camera::{ManualTextureViewHandle, RenderTarget};
use bevy::math::{UVec2, Vec2, Vec3};
use bevy::prelude::*;
use bevy::render::renderer::{
    RenderAdapter, RenderAdapterInfo, RenderDevice, RenderInstance, RenderQueue, WgpuWrapper,
};
use bevy::render::settings::{RenderCreation, RenderResources};
use bevy::render::texture::{ManualTextureView, ManualTextureViews};
use bevy::render::RenderPlugin;
use bevy::window::{ExitCondition, WindowPlugin};

use crate::abi::*;
use crate::gpu::{self, HostGpu};

/// Only one target is ever registered, so the handle is a constant.
const TARGET: ManualTextureViewHandle = ManualTextureViewHandle(0);

/// Marks the entity standing in for the simulation's ball.
#[derive(Component)]
struct BallSprite;

pub struct BevyRenderer {
    _gpu: HostGpu,
    app: App,
    /// Engine-owned; the composite names it every frame.
    texture: *mut c_void,
    /// Kept alive: dropping the texture or its view would take the render
    /// target out from under Bevy.
    target_texture: wgpu::Texture,
    target_view: wgpu::TextureView,
    width: u32,
    height: u32,
}

impl BevyRenderer {
    /// Builds the renderer against the engine's device.
    ///
    /// # Safety
    /// `api` must be the live engine table, and the engine must have graphics.
    pub unsafe fn new(
        api: &ft_engine_api,
        width: u32,
        height: u32,
        arena: Vec2,
        ball_radius: f32,
    ) -> Option<Self> {
        let device_info = (api.gpu_device?)();
        if device_info.is_null() {
            return None;
        }
        let gpu = gpu::adopt(&*device_info)?;

        // The engine allocates the image so it stays an ordinary ft_texture:
        // the same object the composite step draws and the engine retires.
        let desc = ft_texture_desc {
            struct_size: core::mem::size_of::<ft_texture_desc>() as u32,
            pixels: core::ptr::null(),
            width,
            height,
            layers: 1,
            format: 0, // FT_TEXTURE_RGBA8
            mipmaps: false,
            linear_filter: true,
        };
        let texture = (api.texture_create?)(&desc);
        if texture.is_null() {
            return None;
        }

        let mut image = ft_gpu_image {
            struct_size: core::mem::size_of::<ft_gpu_image>() as u32,
            image: core::ptr::null_mut(),
            format: 0,
            width: 0,
            height: 0,
            layers: 0,
            layout: 0,
        };
        if !(api.texture_gpu_image?)(texture, &mut image) {
            return None;
        }

        let target_texture = gpu::wrap_image(&gpu, &image)?;
        let view = target_texture.create_view(&wgpu::TextureViewDescriptor::default());
        let target_view = target_texture.create_view(&wgpu::TextureViewDescriptor::default());

        let mut app = App::new();
        app.add_plugins((
            bevy::app::TaskPoolPlugin::default(),
            bevy::diagnostic::FrameCountPlugin,
            bevy::time::TimePlugin,
            bevy::transform::TransformPlugin,
            WindowPlugin {
                primary_window: None,
                exit_condition: ExitCondition::DontExit,
                close_when_requested: false,
                ..default()
            },
            bevy::asset::AssetPlugin::default(),
            RenderPlugin {
                render_creation: RenderCreation::Manual(RenderResources(
                    RenderDevice::from(gpu.device.clone()),
                    RenderQueue(Arc::new(WgpuWrapper::new(gpu.queue.clone()))),
                    RenderAdapterInfo(WgpuWrapper::new(gpu.adapter.get_info())),
                    RenderAdapter(Arc::new(WgpuWrapper::new(gpu.adapter.clone()))),
                    RenderInstance(Arc::new(WgpuWrapper::new(gpu.instance.clone()))),
                )),
                ..default()
            },
            bevy::image::ImagePlugin::default(),
            bevy::mesh::MeshPlugin,
            bevy::camera::CameraPlugin,
            bevy::core_pipeline::CorePipelinePlugin,
            // SpritePlugin registers the text2d systems, which need the font
            // assets TextPlugin provides.
            bevy::text::TextPlugin,
            bevy::sprite::SpritePlugin,
            bevy::sprite_render::SpriteRenderPlugin,
        ));

        // `App::run()` would do this; `App::update()` does not. Without it
        // RenderPlugin::finish never runs, so the render world never receives
        // the device, the pipeline cache or the error handler it expects.
        // Bevy owns the whole arena image, so its clear colour is what shows
        // through wherever no sprite covers it. Colours here are linear on
        // purpose: the target is UNORM and the engine passes texture values
        // through untouched, so an srgb() colour would land about four times
        // too dark next to anything the engine draws.
        app.insert_resource(ClearColor(Color::linear_rgb(0.05, 0.05, 0.09)));

        app.finish();
        app.cleanup();

        app.world_mut()
            .resource_mut::<ManualTextureViews>()
            .insert(
                TARGET,
                // Not with_default_format: that picks Bevy's default surface
                // format (sRGB), while the engine's image is Rgba8Unorm. A
                // mismatch here makes the render graph fail on a format it
                // cannot write, silently in a module with no log subscriber.
                ManualTextureView {
                    texture_view: view.into(),
                    size: UVec2::new(width, height),
                    view_format: wgpu::TextureFormat::Rgba8Unorm,
                },
            );

        // The camera frames the arena in world units, so the sprites below are
        // positioned in the simulation's own coordinates.
        app.world_mut().spawn((
            Camera2d,
            // In 0.19 the target is its own component rather than a Camera field.
            RenderTarget::TextureView(TARGET),
            Projection::Orthographic(OrthographicProjection {
                scaling_mode: bevy::camera::ScalingMode::Fixed {
                    width: arena.x,
                    height: arena.y,
                },
                ..OrthographicProjection::default_2d()
            }),
            // Bevy's 2D world is Y-up; FrameTee's is Y-down, like the image
            // rows the result is composited into. Flipping the camera rather
            // than every entity keeps the scene authored in the engine's
            // coordinates, so positions handed over by the simulation need no
            // conversion.
            Transform::from_xyz(arena.x * 0.5, arena.y * 0.5, 1000.0)
                .with_scale(Vec3::new(1.0, -1.0, 1.0)),
        ));

        // The arena floor and the ball, as ordinary Bevy sprites.
        app.world_mut().spawn((
            Sprite {
                color: Color::linear_rgb(0.11, 0.12, 0.16),
                custom_size: Some(arena),
                ..default()
            },
            Transform::from_xyz(arena.x * 0.5, arena.y * 0.5, 0.0),
        ));
        // The four walls the engine used to draw. Bevy owns the whole scene now,
        // so anything missing here is simply missing from the viewport.
        let wall_colour = Color::linear_rgb(0.28, 0.34, 0.46);
        const WALL: f32 = 0.25;
        for (centre, size) in [
            (Vec2::new(arena.x * 0.5, WALL * 0.5), Vec2::new(arena.x, WALL)),
            (
                Vec2::new(arena.x * 0.5, arena.y - WALL * 0.5),
                Vec2::new(arena.x, WALL),
            ),
            (Vec2::new(WALL * 0.5, arena.y * 0.5), Vec2::new(WALL, arena.y)),
            (
                Vec2::new(arena.x - WALL * 0.5, arena.y * 0.5),
                Vec2::new(WALL, arena.y),
            ),
        ] {
            app.world_mut().spawn((
                Sprite {
                    color: wall_colour,
                    custom_size: Some(size),
                    ..default()
                },
                Transform::from_xyz(centre.x, centre.y, 0.5),
            ));
        }

        app.world_mut().spawn((
            BallSprite,
            Sprite {
                color: Color::linear_rgb(1.0, 0.83, 0.35),
                custom_size: Some(Vec2::splat(ball_radius * 2.0)),
                ..default()
            },
            Transform::from_xyz(arena.x * 0.5, arena.y * 0.5, 1.0),
        ));

        Some(BevyRenderer {
            app,
            texture,
            target_texture,
            target_view,
            _gpu: gpu,
            width,
            height,
        })
    }

    /// Draws one frame with Bevy and composites it into the engine's viewport.
    ///
    /// # Safety
    /// Must be called from inside the engine's render callback.
    pub unsafe fn render(&mut self, api: &ft_engine_api, ball: Vec2, arena: Vec2) {
        // Move the simulation's state onto the sprite Bevy draws.
        let mut query = self
            .app
            .world_mut()
            .query_filtered::<&mut Transform, With<BallSprite>>();
        let world = self.app.world_mut();
        if let Ok(mut transform) = query.single_mut(world) {
            transform.translation = Vec3::new(ball.x, ball.y, 1.0);
        }

        // Bevy renders here, into the engine's image, on the engine's queue.
        self.app.update();

        // The engine samples the image as it stands now, one quad over the
        // arena. Not the atlas path: that copies its sprites once at creation,
        // so a frame drawn afterwards would never appear.
        if let Some(draw_texture) = api.draw_texture {
            draw_texture(
                3.0,
                self.texture,
                ft_rect {
                    x: 0.0,
                    y: 0.0,
                    w: arena.x,
                    h: arena.y,
                },
                ft_color {
                    r: 1.0,
                    g: 1.0,
                    b: 1.0,
                    a: 1.0,
                },
            );
        }

        let _ = (self.width, self.height);
    }
}
