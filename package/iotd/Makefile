include $(TOPDIR)/rules.mk

PKG_NAME:=iotd
PKG_VERSION:=0.1.0
PKG_RELEASE:=4
PKG_LICENSE:=MIT
PKG_MAINTAINER:=Wael Rekik

include $(INCLUDE_DIR)/package.mk

define Package/iotd
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=IoT gateway status publisher over MQTT
  DEPENDS:=+libmosquitto-ssl +libuci
endef

define Package/iotd/description
  Daemon that reads its UCI configuration and publishes
  the gateway network status to an MQTT broker.
endef

define Package/iotd/conffiles
/etc/config/iotd
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-Wall -Wextra -o $(PKG_BUILD_DIR)/iotd $(PKG_BUILD_DIR)/iotd.c \
		-lmosquitto -luci
endef

define Package/iotd/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/iotd $(1)/usr/bin/
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/iotd.config $(1)/etc/config/iotd
endef

$(eval $(call BuildPackage,iotd))
