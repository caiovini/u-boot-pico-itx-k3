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

echo "==> Build complete! Output artifacts:"
for artifact in u-boot u-boot.bin u-boot-nodtb.bin u-boot.dtb u-boot.itb; do
	if [[ -f "${artifact}" ]]; then
		ls -lh "${artifact}"
	fi
done
