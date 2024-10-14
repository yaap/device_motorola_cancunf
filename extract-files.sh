#!/bin/bash
#
# SPDX-FileCopyrightText: 2016 The CyanogenMod Project
# SPDX-FileCopyrightText: 2017-2024 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

set -e

DEVICE=cancunf
VENDOR=motorola

# Load extract_utils and do some sanity checks
MY_DIR="${BASH_SOURCE%/*}"
if [[ ! -d "${MY_DIR}" ]]; then MY_DIR="${PWD}"; fi

ANDROID_ROOT="${MY_DIR}/../../.."

export TARGET_ENABLE_CHECKELF=true

HELPER="${ANDROID_ROOT}/tools/extract-utils/extract_utils.sh"
if [ ! -f "${HELPER}" ]; then
    echo "Unable to find helper script at ${HELPER}"
    exit 1
fi
source "${HELPER}"

# Default to sanitizing the vendor folder before extraction
CLEAN_VENDOR=true

KANG=
SECTION=

while [ "${#}" -gt 0 ]; do
    case "${1}" in
        -n | --no-cleanup )
                CLEAN_VENDOR=false
                ;;
        -k | --kang )
                KANG="--kang"
                ;;
        -s | --section )
                SECTION="${2}"; shift
                CLEAN_VENDOR=false
                ;;
        * )
                SRC="${1}"
                ;;
    esac
    shift
done

if [ -z "${SRC}" ]; then
    SRC="adb"
fi

function blob_fixup {
    case "$1" in
        vendor/etc/init/android.hardware.neuralnetworks-shim-service-mtk.rc)
            [ "$2" = "" ] && return 0
            sed -i 's/start/enable/' "$2"
            ;;
        vendor/etc/vintf/manifest/manifest_media_c2_V1_2_default.xml)
            [ "$2" = "" ] && return 0
            sed -i 's/1.1/1.2/' "$2"
            ;;
        vendor/bin/hw/android.hardware.media.c2@1.2-mediatek | vendor/bin/hw/android.hardware.media.c2@1.2-mediatek-64b)
            [ "$2" = "" ] && return 0
            "${PATCHELF}" --replace-needed "libavservices_minijail_vendor.so" "libavservices_minijail.so" "${2}"
            grep -q "libstagefright_foundation-v33.so" "${2}" || "${PATCHELF}" --add-needed "libstagefright_foundation-v33.so" "${2}"
            ;;
        vendor/bin/hw/android.hardware.security.keymint-service.trustonic)
            [ "$2" = "" ] && return 0
            "${PATCHELF}" --replace-needed "android.hardware.security.keymint-V1-ndk_platform.so" "android.hardware.security.keymint-V1-ndk.so" "${2}"
            "${PATCHELF}" --replace-needed "android.hardware.security.secureclock-V1-ndk_platform.so" "android.hardware.security.secureclock-V1-ndk.so" "${2}"
            "${PATCHELF}" --replace-needed "android.hardware.security.sharedsecret-V1-ndk_platform.so" "android.hardware.security.sharedsecret-V1-ndk.so" "${2}"
            grep -q "android.hardware.security.rkp-V1-ndk.so" "${2}" || "${PATCHELF}" --add-needed "android.hardware.security.rkp-V1-ndk.so" "${2}"
            ;;
        vendor/lib*/hw/mt6855/vendor.mediatek.hardware.pq@2.15-impl.so \
        |vendor/lib64/mt6855/libmtkcam_stdutils.so \
        |vendor/lib64/hw/mt6855/android.hardware.camera.provider@2.6-impl-mediatek.so \
        |vendor/lib64/hw/android.hardware.thermal@2.0-impl.so)
            "${PATCHELF}" --replace-needed "libutils.so" "libutils-v32.so" "${2}"
            ;;
        vendor/lib*/hw/audio.primary.mediatek.so)
            [ "$2" = "" ] && return 0
            "${PATCHELF}" --replace-needed "libutils.so" "libutils-v32.so" "${2}"
            "${PATCHELF}" --replace-needed "libalsautils.so" "libalsautils-v31.so" "${2}"
            grep -q "libstagefright_foundation-v33.so" "${2}" || "${PATCHELF}" --add-needed "libstagefright_foundation-v33.so" "${2}"
            ;;
        vendor/bin/mnld \
        |vendor/lib64/hw/android.hardware.sensors@2.X-subhal-mediatek.so\
        |vendor/lib64/mt6855/libcam.utils.sensorprovider.so)
            [ "$2" = "" ] && return 0
            grep -q "libshim_sensors.so" "$2" || "$PATCHELF" --add-needed "libshim_sensors.so" "$2"
            ;;
        vendor/lib64/libdlbdsservice.so \
        |vendor/lib64/libcodec2_soft_ddpdec.so \
        |vendor/lib*/soundfx/libswdap.so \
        |vendor/lib*/soundfx/libdlbvol.so \
        |vendor/lib64/libcodec2_soft_ac4dec.so \
        |vendor/bin/hw/vendor.dolby.hardware.dms@2.0-service)
            [ "$2" = "" ] && return 0
            grep -q "libstagefright_foundation-v33.so" "${2}" || "${PATCHELF}" --add-needed "libstagefright_foundation-v33.so" "${2}"
            ;;
        vendor/etc/dolby/dax-default.xml)
            [ "$2" = "" ] && return 0
            sed -i 's|volume-leveler-enable value="true"|volume-leveler-enable value="false"|g' "${2}"
            ;;
        vendor/lib64/libmtkcam_featurepolicy.so)
            [ "$2" = "" ] && return 0
            # evaluateCaptureConfiguration()
            printf '\x28\x02\x80\x52' | dd of="$2" bs=1 seek=$((0x3e828)) count=4 conv=notrunc
            printf '\x28\x02\x80\x52' | dd of="$2" bs=1 seek=$((0x3e8f4)) count=4 conv=notrunc
            ;;
        vendor/etc/init/android.hardware.bluetooth@1.1-service-mediatek.rc)
            [ "$2" = "" ] && return 0
            sed -i '/vts/Q' "$2"
            ;;
        vendor/lib64/sensors.moto.so)
            [ "$2" = "" ] && return 0
            "${PATCHELF}" --replace-needed "libutils.so" "libutils-v32.so" "${2}"
            ;;
        system_ext/lib64/libsource.so)
            [ "$2" = "" ] && return 0
            grep -q libui_shim.so "$2" || "$PATCHELF" --add-needed libui_shim.so "$2"
            ;;
        vendor/lib64/hw/android.hardware.gnss-impl-mediatek.so \
        |vendor/bin/hw/android.hardware.gnss-service.mediatek)
            [ "$2" = "" ] && return 0
            "${PATCHELF}" --replace-needed "android.hardware.gnss-V1-ndk_platform.so" "android.hardware.gnss-V1-ndk.so" "${2}"
            ;;
        vendor/bin/hw/android.hardware.memtrack-service.mediatek)
            [ "$2" = "" ] && return 0
            "${PATCHELF}" --replace-needed "android.hardware.memtrack-V1-ndk_platform.so" "android.hardware.memtrack-V1-ndk.so" "${2}"
            ;;
        vendor/lib64/mt6855/lib3a.flash.so \
        |vendor/lib64/mt6855/lib3a.ae.stat.so \
        |vendor/lib64/mt6855/lib3a.sensors.flicker.so \
        |vendor/lib64/mt6855/lib3a.sensors.color.so \
        |vendor/lib64/mt6855/libaaa_ltm.so \
        |vendor/lib64/lib3a.ae.pipe.so \
        |vendor/lib64/libSQLiteModule_VER_ALL.so)
            [ "$2" = "" ] && return 0
            grep -q "liblog.so" "${2}" || "${PATCHELF_0_17_2}" --add-needed "liblog.so" "${2}"
            ;;
        vendor/lib64/mt6855/libmnl.so)
            [ "$2" = "" ] && return 0
            grep -q "libcutils.so" "${2}" || "${PATCHELF}" --add-needed "libcutils.so" "${2}"
            ;;
        *)
            return 1
            ;;
    esac

    return 0
}

function blob_fixup_dry() {
    blob_fixup "$1" ""
}

# Initialize the helper
setup_vendor "${DEVICE}" "${VENDOR}" "${ANDROID_ROOT}" false "${CLEAN_VENDOR}"

extract "${MY_DIR}/proprietary-files.txt" "${SRC}" "${KANG}" --section "${SECTION}"

"${MY_DIR}/setup-makefiles.sh"
