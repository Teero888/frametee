//! Adopting FrameTee's graphics device.
//!
//! The engine renders with Vulkan and hands its device over through
//! `ft_engine_api::gpu_device`. wgpu can be built on top of an existing Vulkan
//! device rather than creating one of its own, so Bevy's renderer runs on the
//! engine's device and draws straight into the engine's images. Nothing is
//! copied and no pixels cross the boundary: the only things that travel are the
//! handles below.
//!
//! Everything here is unsafe by nature. The contract being relied on is:
//!
//!  * The engine owns the instance, device and queue and outlives this module.
//!    Every `drop_callback` is therefore `Some`, which is the way wgpu is told
//!    a handle is borrowed: it destroys anything whose drop guard is `None`, so
//!    passing `None` here would have wgpu call `vkDestroyDevice` on the
//!    engine's device the moment this module is unloaded.
//!  * The image belongs to an `ft_texture` the module created, and the engine
//!    keeps it in `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL` between frames.
//!  * Work submitted here goes to the engine's own queue, so it is ordered
//!    ahead of the engine's frame without any semaphore of our own.

use ash::vk::{self, Handle};

use crate::abi::{ft_gpu_device, ft_gpu_image, FT_GPU_API_VULKAN};

/// The engine's device, expressed as wgpu objects Bevy can be handed.
pub struct HostGpu {
    pub instance: wgpu::Instance,
    pub adapter: wgpu::Adapter,
    pub device: wgpu::Device,
    pub queue: wgpu::Queue,
}

/// Builds wgpu on top of the engine's Vulkan device.
///
/// # Safety
/// Every handle in `dev` must be live and must outlive the returned `HostGpu`.
pub unsafe fn adopt(dev: &ft_gpu_device) -> Option<HostGpu> {
    if dev.api != FT_GPU_API_VULKAN || dev.instance.is_null() || dev.device.is_null() {
        return None;
    }
    
    let entry = ash::Entry::load().ok()?;
    let raw_instance = ash::Instance::load(
        entry.static_fn(),
        vk::Instance::from_raw(dev.instance as u64),
    );

    // No extensions are declared because the engine created the instance and
    // this module cannot add to it after the fact. wgpu is therefore limited to
    // core functionality, which is all a render-to-texture path needs.
    let hal_instance = wgpu::hal::vulkan::Instance::from_raw(
        entry,
        raw_instance,
        dev.api_version,
        0,
        None,
        Vec::new(),
        wgpu::InstanceFlags::empty(),
        wgpu::MemoryBudgetThresholds::default(),
        false,
        Some(Box::new(|| {})),
    )
    .ok()?;

    let exposed =
        hal_instance.expose_adapter(vk::PhysicalDevice::from_raw(dev.physical_device as u64))?;

    let raw_device = ash::Device::load(
        hal_instance.shared_instance().raw_instance().fp_v1_0(),
        vk::Device::from_raw(dev.device as u64),
    );

    // Only what the engine's device was actually created with may be claimed
    // here: a device cannot gain extensions or features after creation, and
    // promising wgpu something the device does not have is how this path turns
    // into a driver crash rather than an error.
    let limits = exposed.capabilities.limits.clone();
    let open = exposed
        .adapter
        .device_from_raw(
            raw_device,
            Some(Box::new(|| {})),
            &[],
            wgpu::Features::empty(),
            &limits,
            &wgpu::MemoryHints::default(),
            dev.queue_family_index,
            0,
        )
        .ok()?;

    let instance = wgpu::Instance::from_hal::<wgpu::hal::api::Vulkan>(hal_instance);
    let adapter = instance.create_adapter_from_hal(exposed);
    let (device, queue) = adapter
        .create_device_from_hal(
            open,
            &wgpu::DeviceDescriptor {
                label: Some("frametee host device"),
                required_features: wgpu::Features::empty(),
                required_limits: limits,
                ..Default::default()
            },
        )
        .ok()?;

    Some(HostGpu {
        instance,
        adapter,
        device,
        queue,
    })
}

/// Wraps an engine-owned image as a wgpu texture Bevy can render into.
///
/// # Safety
/// `image` must describe a live `ft_texture` that this module owns, and the
/// texture must outlive the returned handle.
pub unsafe fn wrap_image(gpu: &HostGpu, image: &ft_gpu_image) -> Option<wgpu::Texture> {
    if image.image.is_null() || image.width == 0 || image.height == 0 {
        return None;
    }

    let format = wgpu::TextureFormat::Rgba8Unorm;
    let size = wgpu::Extent3d {
        width: image.width,
        height: image.height,
        depth_or_array_layers: image.layers.max(1),
    };
    let usage = wgpu::TextureUses::COLOR_TARGET | wgpu::TextureUses::RESOURCE;

    let hal_desc = wgpu::hal::TextureDescriptor {
        label: Some("frametee game target"),
        size,
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format,
        usage,
        memory_flags: wgpu::hal::MemoryFlags::empty(),
        view_formats: Vec::new(),
    };

    // The drop guard keeps wgpu from calling `vkDestroyImage`, and `External`
    // keeps it from freeing the memory: the engine allocated both and retires
    // them on its own schedule.
    let hal_texture = {
        let hal_device = gpu.device.as_hal::<wgpu::hal::api::Vulkan>()?;
        hal_device.texture_from_raw(
            vk::Image::from_raw(image.image as u64),
            &hal_desc,
            Some(Box::new(|| {})),
            wgpu::hal::vulkan::TextureMemory::External,
        )
    };

    Some(gpu.device.create_texture_from_hal::<wgpu::hal::api::Vulkan>(
        hal_texture,
        &wgpu::TextureDescriptor {
            label: Some("frametee game target"),
            size,
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        },
    ))
}
