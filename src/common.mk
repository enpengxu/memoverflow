ifndef QCONFIG
QCONFIG=qconfig.mk
endif
include $(QCONFIG)

USEFILE=/dev/null

NAME=mozart

include $(MKFILES_ROOT)/qtargets.mk
