human_arch	= ARM (hard float)
build_arch	= arm
header_arch	= arm
defconfig	= defconfig
#flavours	= omap highbank
flavours	= tegra
build_image	= zImage
kernel_file	= arch/$(build_arch)/boot/zImage
install_file	= vmlinuz
no_dumpfile	= true

loader		= grub
do_tools	= false

# Flavour specific configuration.
#dtb_file_highbank	= arch/$(build_arch)/boot/highbank.dtb
dtb_files_omap	= imx6q-sabrelite.dtb omap3-beagle-xm.dtb omap4-panda.dtb omap4-panda-es.dtb
dtb_files_tegra = tegra20-harmony.dtb tegra20-paz00.dtb tegra20-seaboard.dtb tegra20-trimslice.dtb tegra20-whistler.dtb tegra30-cardhu-a04.dtb \
		  tegra20-medcom-wide.dtb tegra20-plutux.dtb tegra20-tec.dtb tegra20-ventana.dtb tegra30-cardhu-a02.dtb
