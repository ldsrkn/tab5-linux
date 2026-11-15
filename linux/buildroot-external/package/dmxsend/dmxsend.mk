################################################################################
# DMX512 sender for the Tab5 Grove port (M5Stack Unit DMX)
################################################################################

DMXSEND_SITE = \
	$(BR2_EXTERNAL_EASYSTICK_STAMP_P4_PATH)/package/dmxsend/src
DMXSEND_SITE_METHOD = local
DMXSEND_LICENSE = GPL-2.0-only

define DMXSEND_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 -Wall -Wextra -Werror \
		$(TARGET_LDFLAGS) -o $(@D)/dmxsend \
		$(@D)/dmxsend.c
endef

define DMXSEND_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/dmxsend \
		$(TARGET_DIR)/usr/bin/dmxsend
endef

$(eval $(generic-package))
