# Dawn RT

This is a fork of Dawn which gets extended with a Ray-Tracing extension. The extension is implemented into the Vulkan backend (using *VK_KHR_ray_tracing*) and the D3D12 backend (using *DXR*).

The specification of the Ray-Tracing extension can be found [here](https://github.com/maierfelix/dawn-ray-tracing/blob/master/RT_SPEC.md).

The relative WebGPU issue can be found [here](https://github.com/gpuweb/gpuweb/issues/535).

An article covering the basic usage of the API can be found [here](http://maierfelix.github.io/2020-01-13-webgpu-ray-tracing/).

## Preview

<img src="https://i.imgur.com/VgPB36q.png" width="488"><br/>
*Path Tracing*<br/>
<img src="https://i.imgur.com/BliBL3i.png" width="488"><br/>
*Procedural Geometry*

## ‌‌ 

Dawn is an open-source and cross-platform implementation of the work-in-progress [WebGPU](https://webgpu.dev) standard.
More precisely it implements [`webgpu.h`](https://github.com/webgpu-native/webgpu-headers/blob/master/webgpu.h) that is a one-to-one mapping with the WebGPU IDL.
Dawn is meant to be integrated as part of a larger system and is the underlying implementation of WebGPU in Chromium.

Dawn provides several WebGPU building blocks:
 - **WebGPU C/C++ headers** that applications and other building blocks use.
   - The `webgpu.h` version that Dawn implements.
   - A C++ wrapper for the `webgpu.h`.
 - **A "native" implementation of WebGPU** using platforms' GPU APIs:
   - **D3D12** on Windows 10
   - **Metal** on macOS and iOS
   - **Vulkan** on Windows, Linux, ChromeOS, Android and Fuchsia
   - OpenGL as best effort where available
 - **A client-server implementation of WebGPU** for applications that are in a sandbox without access to native drivers

Helpful links:

 - [Dawn's bug tracker](https://bugs.chromium.org/p/dawn/issues/entry) if you find issues with Dawn.
 - [Dawn's mailing list](https://groups.google.com/forum/#!members/dawn-graphics) for other discussions related to Dawn.
 - [Dawn's source code](https://dawn.googlesource.com/dawn)
 - [Dawm's Matrix chatroom](https://matrix.to/#/#webgpu-dawn:matrix.org) for live discussion around contributing or using Dawn.
 - [WebGPU's Matrix chatroom](https://matrix.to/#/#WebGPU:matrix.org)

## Documentation table of content

Developer documentation:

 - [Dawn overview](docs/overview.md)
 - [Building Dawn](docs/buiding.md)
 - [Contributing to Dawn](CONTRIBUTING.md)
 - [Testing Dawn](docs/testing.md)
 - [Debugging Dawn](docs/debugging.md)
 - [Dawn's infrastructure](docs/infra.md)

User documentation: (TODO, figure out what overlaps with webgpu.h docs)

## Status

(TODO)

## License

Apache 2.0 Public License, please see [LICENSE](/LICENSE).

## Disclaimer

This is not an officially supported Google product.
