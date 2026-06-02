# VibeOS Web Emulator Assets

`docs/index.html` looks for these files when published through GitHub Pages:

- `docs/assets/vibeos.iso`
- `docs/assets/vibeos-disk.img`

The disk image is optional. You can also keep large artifacts out of git and pass release URLs instead:

```text
https://<user>.github.io/<repo>/?iso=https://.../vibeos.iso&disk=https://.../vibeos-disk.img
```

Current limitation: the bundled static page uses v86, which does not support x86_64 long mode. VibeOS currently enters long mode during boot, so this page is ready as a GitHub Pages shell but needs an x86_64-capable browser emulator backend, or a 32-bit VibeOS boot target, before the OS can fully boot in-browser.
