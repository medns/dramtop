# PawnIO IntelMCHBAR module

`IntelMCHBAR.bin` is the signed IntelMCHBAR module from [PawnIO.Modules](https://github.com/namazso/PawnIO.Modules) release [0.2.11](https://github.com/namazso/PawnIO.Modules/releases/tag/0.2.11), unchanged (SHA-256 `3f82b832d99b4aac37d2a20fdb7c9baa2a3bc0488612c9019c9484eb0e8a6eae`). Its source is [`IntelMCHBAR.p`](https://github.com/namazso/PawnIO.Modules/blob/0.2.11/IntelMCHBAR.p). It is licensed under the GNU Lesser General Public License, version 2.1 or later; see `COPYING`.

The Windows build embeds it as a resource. When the [PawnIO](https://pawnio.eu/) driver is installed, dramtop loads the module to read the memory controller's counters; the driver accepts only modules signed by its author. To update the module, replace the file with the one from a newer release and update the version and checksum above.
