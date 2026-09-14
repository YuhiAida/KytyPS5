#!/usr/bin/env bash
# probe-bc-blit.sh - report Vulkan block-compressed (BC) format capabilities of the host GPU.
#
# Why: the render-scale feature can only resample compressed textures if the device supports
# BLIT_DST for the BC format (upload blits staging -> scaled backing, so the scaled BC image is the
# destination). The Vulkan mandatory-format table guarantees BLIT_SRC for BC, but NOT BLIT_DST.
# It also checks vkCreateImage for BC at extents that are not a multiple of the 4x4 block size
# (VUID-VkImageCreateInfo-extent-02252), because render-scale rounds scaled extents to pixels.
#
# Usage: tools/probe-bc-blit.sh [-h] [--formats] [--json]
#   -h, --help   show this help
#   --formats    list every format the probe checks and exit
#   --json       emit machine-readable lines (key=value) instead of the table
#
# Output is compact on purpose (it becomes agent context). Exit codes: 0 ok, 1 probe failed,
# 2 bad usage.
#
# Runs on the host: inside the VS Code flatpak it re-execs itself with flatpak-spawn.
set -euo pipefail

SCRIPT_PATH="$(readlink -f "${BASH_SOURCE[0]}")"
REPO_ROOT="$(cd "$(dirname "$SCRIPT_PATH")/.." && pwd)"

usage() {
	sed -n '2,16p' "$SCRIPT_PATH" | sed 's/^# \{0,1\}//'
}

JSON=0
for arg in "$@"; do
	case "$arg" in
		-h | --help)
			usage
			exit 0
			;;
		--json) JSON=1 ;;
		--formats)
			printf '%s\n' BC1_RGBA_UNORM BC1_RGBA_SRGB BC3_UNORM BC3_SRGB BC4_UNORM BC5_UNORM \
				BC6H_UFLOAT BC6H_SFLOAT BC7_UNORM BC7_SRGB R8G8B8A8_UNORM
			exit 0
			;;
		*)
			echo "probe-bc-blit.sh: unknown option: $arg" >&2
			usage >&2
			exit 2
			;;
	esac
done

# The Vulkan driver lives on the host, not in the sandbox.
if [[ "${container:-}" == "flatpak" ]] && command -v flatpak-spawn >/dev/null 2>&1; then
	quoted_args=""
	if (($# > 0)); then
		printf -v quoted_args '%q ' "$@"
	fi
	exec flatpak-spawn --host bash -lc "exec \"$SCRIPT_PATH\" $quoted_args"
fi

for tool in g++ pkg-config; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "probe-bc-blit.sh: missing tool: $tool" >&2
		exit 1
	fi
done

VULKAN_INCLUDE="$REPO_ROOT/3rdparty/Vulkan-Headers/include"
if [[ ! -f "$VULKAN_INCLUDE/vulkan/vulkan.h" ]]; then
	echo "probe-bc-blit.sh: Vulkan headers not found at $VULKAN_INCLUDE" >&2
	exit 1
fi

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

cat >"$WORK_DIR/probe.cpp" <<'EOF'
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>

struct FormatEntry {
	const char* name;
	VkFormat format;
};

static const FormatEntry kFormats[] = {
    {"BC1_RGBA_UNORM", VK_FORMAT_BC1_RGBA_UNORM_BLOCK},
    {"BC1_RGBA_SRGB", VK_FORMAT_BC1_RGBA_SRGB_BLOCK},
    {"BC3_UNORM", VK_FORMAT_BC3_UNORM_BLOCK},
    {"BC3_SRGB", VK_FORMAT_BC3_SRGB_BLOCK},
    {"BC4_UNORM", VK_FORMAT_BC4_UNORM_BLOCK},
    {"BC5_UNORM", VK_FORMAT_BC5_UNORM_BLOCK},
    {"BC6H_UFLOAT", VK_FORMAT_BC6H_UFLOAT_BLOCK},
    {"BC6H_SFLOAT", VK_FORMAT_BC6H_SFLOAT_BLOCK},
    {"BC7_UNORM", VK_FORMAT_BC7_UNORM_BLOCK},
    {"BC7_SRGB", VK_FORMAT_BC7_SRGB_BLOCK},
    {"R8G8B8A8_UNORM", VK_FORMAT_R8G8B8A8_UNORM},
};

// Only the bits the render-scale resample path depends on.
static void Decode(VkFormatFeatureFlags f, char* out, size_t out_size) {
	snprintf(out, out_size, "%s%s%s%s", (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? "SAMPLED " : "",
	         (f & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ? "BLIT_SRC " : "",
	         (f & VK_FORMAT_FEATURE_BLIT_DST_BIT) ? "BLIT_DST " : "",
	         (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? "LINEAR" : "");
}

static const char* ResultName(VkResult r) {
	switch (r) {
		case VK_SUCCESS: return "OK";
		case VK_ERROR_FORMAT_NOT_SUPPORTED: return "FORMAT_NOT_SUPPORTED";
		case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "OUT_OF_DEVICE_MEMORY";
		case VK_ERROR_OUT_OF_HOST_MEMORY: return "OUT_OF_HOST_MEMORY";
		default: return "ERROR";
	}
}

int main(int argc, char** argv) {
	const int json = (argc > 1 && strcmp(argv[1], "--json") == 0);

	VkApplicationInfo app {};
	app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.apiVersion         = VK_API_VERSION_1_1;
	app.pApplicationName   = "kyty-format-probe";
	app.applicationVersion = 1;

	VkInstanceCreateInfo ici {};
	ici.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	ici.pApplicationInfo = &app;

	VkInstance instance = VK_NULL_HANDLE;
	if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
		printf("probe: vkCreateInstance failed\n");
		return 1;
	}

	uint32_t device_count = 0;
	vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
	if (device_count == 0) {
		printf("probe: no Vulkan physical device\n");
		vkDestroyInstance(instance, nullptr);
		return 1;
	}
	VkPhysicalDevice* devices = new VkPhysicalDevice[device_count];
	vkEnumeratePhysicalDevices(instance, &device_count, devices);

	const VkPhysicalDevice device = devices[0];
	VkPhysicalDeviceProperties props {};
	vkGetPhysicalDeviceProperties(device, &props);
	if (!json) {
		printf("device=%s type=%d api=%u.%u.%u max2D=%u\n", props.deviceName, props.deviceType,
		       VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
		       VK_VERSION_PATCH(props.apiVersion), props.limits.maxImageDimension2D);
	}

	// Image-creation tests need a logical device.
	uint32_t family_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, nullptr);
	VkQueueFamilyProperties* families = new VkQueueFamilyProperties[family_count];
	vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, families);
	uint32_t family_index = 0;
	for (uint32_t i = 0; i < family_count; i++) {
		if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
			family_index = i;
			break;
		}
	}
	delete[] families;

	const float priority = 1.0f;
	VkDeviceQueueCreateInfo queue_info {};
	queue_info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_info.queueFamilyIndex = family_index;
	queue_info.queueCount       = 1;
	queue_info.pQueuePriorities = &priority;

	VkDeviceCreateInfo device_info {};
	device_info.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos    = &queue_info;

	VkDevice logical = VK_NULL_HANDLE;
	if (vkCreateDevice(device, &device_info, nullptr, &logical) != VK_SUCCESS) {
		printf("probe: vkCreateDevice failed\n");
		vkDestroyInstance(instance, nullptr);
		return 1;
	}

	int blit_dst_missing = 0;
	int unaligned_create_failed = 0;

	for (const auto& entry : kFormats) {
		VkFormatProperties fprops {};
		vkGetPhysicalDeviceFormatProperties(device, entry.format, &fprops);
		char decoded[64] = {};
		Decode(fprops.optimalTilingFeatures, decoded, sizeof(decoded));
		if (json) {
			printf("format %s optimal=0x%08x blit_src=%d blit_dst=%d linear=%d\n", entry.name,
			       static_cast<unsigned>(fprops.optimalTilingFeatures),
			       (fprops.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ? 1 : 0,
			       (fprops.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) ? 1 : 0,
			       (fprops.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? 1 : 0);
		} else {
			printf("  %-16s optimal=0x%08x %s\n", entry.name,
			       static_cast<unsigned>(fprops.optimalTilingFeatures), decoded);
		}
		if (strncmp(entry.name, "BC", 2) == 0 &&
		    !(fprops.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
			blit_dst_missing++;
		}
	}

	// Scaled render-scale extents are pixel-rounded, so a compressed image can end up with an
	// extent that is not a multiple of the 4x4 block size. Ask the driver whether it cares.
	struct Extent {
		uint32_t width;
		uint32_t height;
	};
	const Extent kExtents[] = {{1280, 720}, {1920, 1080}, {1366, 768}, {1367, 769}, {4096, 4096}};
	const VkFormat kCreateFormats[] = {VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK,
	                                   VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_R8G8B8A8_UNORM};

	for (VkFormat format : kCreateFormats) {
		const char* format_name = "R8G8B8A8_UNORM";
		switch (format) {
			case VK_FORMAT_BC7_UNORM_BLOCK: format_name = "BC7_UNORM"; break;
			case VK_FORMAT_BC5_UNORM_BLOCK: format_name = "BC5_UNORM"; break;
			case VK_FORMAT_BC6H_UFLOAT_BLOCK: format_name = "BC6H_UFLOAT"; break;
			default: break;
		}
		VkFormatProperties fprops {};
		vkGetPhysicalDeviceFormatProperties(device, format, &fprops);
		const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
		                                VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		for (const auto& extent : kExtents) {
			VkImageCreateInfo info {};
			info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			info.imageType     = VK_IMAGE_TYPE_2D;
			info.format        = format;
			info.extent        = {extent.width, extent.height, 1};
			info.mipLevels     = 1;
			info.arrayLayers   = 1;
			info.samples       = VK_SAMPLE_COUNT_1_BIT;
			info.tiling        = VK_IMAGE_TILING_OPTIMAL;
			info.usage         = usage;
			info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
			info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

			VkImage image = VK_NULL_HANDLE;
			const VkResult result = vkCreateImage(logical, &info, nullptr, &image);
			if (image != VK_NULL_HANDLE) {
				vkDestroyImage(logical, image, nullptr);
			}
			const int aligned = (extent.width % 4u == 0u && extent.height % 4u == 0u);
			if (result != VK_SUCCESS && !aligned) {
				unaligned_create_failed++;
			}
			if (json) {
				printf("create %s %ux%u aligned=%d result=%s\n", format_name, extent.width,
				       extent.height, aligned, ResultName(result));
			} else {
				printf("  create %-16s %4ux%-4u aligned=%-3s %s\n", format_name, extent.width,
				       extent.height, aligned ? "yes" : "no", ResultName(result));
			}
		}
	}

	vkDestroyDevice(logical, nullptr);
	vkDestroyInstance(instance, nullptr);
	delete[] devices;

	// Verdict lines are the point of the probe: the resample path needs BLIT_DST on compressed
	// formats, and the scaled host extent must be legal for the driver.
	printf("VERDICT bc_blit_dst=%s bc_unaligned_extent=%s\n", blit_dst_missing == 0 ? "YES" : "NO",
	       unaligned_create_failed == 0 ? "accepted" : "rejected");
	return 0;
}
EOF

if ! g++ -std=c++17 -O1 -I"$VULKAN_INCLUDE" "$WORK_DIR/probe.cpp" -o "$WORK_DIR/probe" -lvulkan 2>"$WORK_DIR/build.log"; then
	echo "probe-bc-blit.sh: compile failed:" >&2
	tail -20 "$WORK_DIR/build.log" | cut -c1-200 >&2
	exit 1
fi

if [[ "$JSON" == "1" ]]; then
	"$WORK_DIR/probe" --json | cut -c1-200
else
	"$WORK_DIR/probe" | cut -c1-200
fi
