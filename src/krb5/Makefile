# This is a Makefile fragment, see ../Makefile

K5LIBS := $(shell pkg-config --libs-only-l krb5-gssapi)
K5LDFLAGS := $(shell pkg-config --libs-only-L krb5-gssapi)

SRC_DIRS += $(PVACCESS_SRC)/krb5

pvAccess_SRCS_Linux += krb5auth.cpp

pvAccess_SYS_LIBS_Linux += $(K5LIBS:-l%=%)

USR_LDFLAGS_Linux += $(K5LDFLAGS)
