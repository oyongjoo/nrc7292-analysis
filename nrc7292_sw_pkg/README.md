# NRC7292 Software Package

This is the NRC7292 HaLow (IEEE 802.11ah) Linux kernel driver package.

## Package Structure

- `package/src/nrc/` - Linux kernel driver source code
- `package/src/cli_app/` - CLI application for driver control
- `package/src/ft232h-usb-spi/` - FT232H USB-SPI bridge driver (optional)
- `package/evk/` - Evaluation kit files and scripts

## Build Instructions

### Kernel Driver
```bash
cd package/src/nrc
make clean
make
```

### CLI Application  
```bash
cd package/src/cli_app
make clean
make
```

## Documentation

For detailed analysis and documentation, see the main repository documentation in the `code_analysis/` directory.