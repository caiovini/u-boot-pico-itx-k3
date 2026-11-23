#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

export ARCH=riscv
export CROSS_COMPILE="${CROSS_COMPILE:-riscv64-linux-gnu-}"
DEFCONFIG="${DEFCONFIG:-spacemit_k3_defconfig}"
JOBS="${JOBS:-$(nproc)}"
export OPENSBI=./u-boot.bin
if [[ -z "${OPENSBI:-}" && ! -f "fw_dynamic.bin" ]]; then
	export BINMAN_ALLOW_MISSING="${BINMAN_ALLOW_MISSING:-1}"
fi

usage() {
	cat <<EOF
Usage: $0 [clean|distclean|defconfig|menuconfig|all]

Environment variables:
  CROSS_COMPILE  Cross-compiler prefix (default: riscv64-linux-gnu-)
  DEFCONFIG      Board defconfig (default: spacemit_k3_defconfig)
  JOBS           Parallel build jobs (default: $(nproc))
EOF
}

if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
	echo "Error: Cross-compiler '${CROSS_COMPILE}gcc' not found in PATH." >&2
	exit 1
fi

ACTION="${1:-all}"

case "${ACTION}" in
	-h|--help|help)
		usage
		exit 0
		;;
	clean)
		make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" clean
		exit 0
		;;
	distclean)
		make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" distclean
		exit 0
		;;
	defconfig)
		make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" "${DEFCONFIG}"
		exit 0
		;;
	menuconfig)
		make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" menuconfig
		exit 0
		;;
	all)
		;;
	*)
		echo "Unknown action: ${ACTION}" >&2
		usage
		exit 1
		;;
esac

echo "==> Configuring U-Boot with ${DEFCONFIG}..."
make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" "${DEFCONFIG}"

echo "==> Building U-Boot (${JOBS} jobs)..."
make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" -j"${JOBS}"
if [[ -f "dts/dt.dtb" ]]; then
	cp -f dts/dt.dtb u-boot.dtb
fi

# Package SPI-NOR-compatible external-data (-E -B 8) u-boot.itb with crc32 hashes
if [[ -f "u-boot.bin" && -f "firmware/opensbi_fdt.dtb" ]]; then
	TMP_ITS="$(mktemp /tmp/k3_spi_uboot.XXXXXX.its)"
	cat > "${TMP_ITS}" <<EOF
/dts-v1/;
/ {
	description = "U-boot FIT image for k3";
	#address-cells = <2>;
	fit,fdt-list = "of-list";
	images {
		uboot {
			description = "U-Boot";
			type = "standalone";
			os = "u-boot";
			arch = "riscv";
			compression = "none";
			load = <0x1 0x02000000>;
			data = /incbin/("${SCRIPT_DIR}/u-boot.bin");
			hash-1 { algo = "crc32"; };
		};
		fdt_11 {
			description = "k3-pico-itx.dtb";
			type = "flat_dt";
			compression = "none";
			data = /incbin/("${SCRIPT_DIR}/firmware/opensbi_fdt.dtb");
			hash-1 { algo = "crc32"; };
		};
	};
	configurations {
		default = "conf_11";
		conf_1  { description = "k3_fpga";     loadables = "uboot"; fdt = "fdt_11"; };
		conf_2  { description = "k3_evb";      loadables = "uboot"; fdt = "fdt_11"; };
		conf_3  { description = "k3_deb1";     loadables = "uboot"; fdt = "fdt_11"; };
		conf_4  { description = "k3_com260";   loadables = "uboot"; fdt = "fdt_11"; };
		conf_5  { description = "k3-deepcomputing-fml13v05"; loadables = "uboot"; fdt = "fdt_11"; };
		conf_6  { description = "k3_itx";      loadables = "uboot"; fdt = "fdt_11"; };
		conf_7  { description = "k3_gemini_c0";loadables = "uboot"; fdt = "fdt_11"; };
		conf_8  { description = "k3_gemini_c1";loadables = "uboot"; fdt = "fdt_11"; };
		conf_9  { description = "k3_evb2-1";   loadables = "uboot"; fdt = "fdt_11"; };
		conf_10 { description = "k3_evb2-2";   loadables = "uboot"; fdt = "fdt_11"; };
		conf_11 { description = "k3-pico-itx"; loadables = "uboot"; fdt = "fdt_11"; };
	};
};
EOF
	mkimage -f "${TMP_ITS}" -E -B 8 "${SCRIPT_DIR}/u-boot.itb" >/dev/null
	rm -f "${TMP_ITS}"
fi

echo "==> Build complete! Output artifacts:"
for artifact in u-boot u-boot.bin u-boot-nodtb.bin u-boot.dtb u-boot.itb; do
	if [[ -f "${artifact}" ]]; then
		ls -lh "${artifact}"
	fi
done
