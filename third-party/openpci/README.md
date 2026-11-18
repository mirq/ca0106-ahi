# openpci.library (third party)

`ca0106.audio` needs `openpci.library` in `LIBS:`. Both implementations are
included here **as the original, unmodified distribution archives**, for
convenience. They are not part of this project and are not covered by its
GPL licence.

| Archive | What | Author | Licence |
| --- | --- | --- | --- |
| `openpci68k-40.16.1.lha` | openpci.library 40.16 for 68k: Prometheus, Prometheus FireStorm, Grex, Mediator; installer, `lspci`, `PCIInit`, docs, SDK | Thomas Richter | THOR-Software Licence v3 (in `openpci68k.readme` inside the archive): free for non-commercial distribution, only verbatim as the complete archive |
| `OpenPci2.1-SDK290208.zip` | OpenPCI 2.1 (68k + MorphOS) library, SDK, includes, docs, example sources | Benjamin Vernoux | no licence text in the archive; freely distributed SDK |

Which one:

* **OpenPCI 2.1** is what `ca0106.audio` was developed and tested with
  (Prometheus FireStorm + Prometheus.card providing the DMA area).
* **openpci.library 40.16** (Thomas Richter) works without a bridge-specific
  library. `ca0106.audio` supports it but it is **untested**; its DMA memory
  from the graphics card needs P96 3.5.0 or newer with `GRANTDIRECTACCESS=Yes`.

Do not install both. Follow the installer and documentation in the archive.

SHA-256:

```
9d360a59e41509092b952987888da34530b8b11d7e7d273f774d2e67e8e7f87e  openpci68k-40.16.1.lha
667e1607e3430733a0d02a874fe3cc7296bfc1470d1fb021ca907991798c748c  OpenPci2.1-SDK290208.zip
```
