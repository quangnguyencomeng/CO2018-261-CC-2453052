# demacOprx

Operating system simulator for HCMUT's CO2018 course. The DMA testbench uses Verilator RTL.

## Run the OS simulator

Run these commands from the repository root. Pass the name of a configuration in `input/` to `./os`.

```bash
make os
./os os_1_singleCPU_mlq_paging
```

## Run the DMA testbench

```bash
make dma
./dma_testbench
./dma_testbench 0x1000 0x2000 1024
```

## Course resources

- The course guide links to Ubuntu VM images for x86 and AArch64. HCMUT SSO is required. [CO2018 course guide](https://docs.google.com/document/d/1vCxl0sBotFjONqRJBAlnkTVYtlL1XZQrTyVl_dO0zxg/edit?usp=sharing)
- [Member 4 memory operations and test status](docs/member4.md)

## License

Educational use for CO2018. See [LICENSE](LICENSE).
