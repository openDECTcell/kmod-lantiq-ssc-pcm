# SPDX-License-Identifier: GPL-2.0-only
#
# OpenWrt package for the Lantiq VR9 SSC PCM/TDM transport.
#
# A generic transport driver for the Lantiq VR9 Synchronous Serial Controller
# (SSC) used as a PCM/TDM slave. Not specific to any peer device.
#
# Drop this directory into an OpenWrt feed or build it out of tree with the
# SDK (tools/build.sh); it produces lantiq_ssc_pcm.ko and /dev/ssc-pcm0.
#
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=lantiq-ssc-pcm
PKG_VERSION:=0.1
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

SSC_PCM_SRC:=$(abspath $(CURDIR)/src)

include $(INCLUDE_DIR)/package.mk

define KernelPackage/lantiq-ssc-pcm
  SUBMENU:=Other modules
  TITLE:=Lantiq VR9 SSC PCM/TDM transport
  DEPENDS:=@TARGET_lantiq
  FILES:=$(PKG_BUILD_DIR)/lantiq_ssc_pcm.ko
  # No AUTOLOAD: the transport is loaded explicitly by whoever uses it.
endef

define KernelPackage/lantiq-ssc-pcm/description
  Exposes the Lantiq VR9 Synchronous Serial Controller used as a PCM/TDM
  slave (peer-supplied clock and frame sync) through /dev/ssc-pcm0.
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)/include/uapi
	$(CP) $(SSC_PCM_SRC)/*.c $(SSC_PCM_SRC)/*.h $(SSC_PCM_SRC)/Makefile \
		$(PKG_BUILD_DIR)/
	$(CP) $(SSC_PCM_SRC)/include/uapi/ssc_pcm.h $(PKG_BUILD_DIR)/include/uapi/
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,lantiq-ssc-pcm))
