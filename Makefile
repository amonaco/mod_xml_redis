BASE=../../../..
# LOCAL_OBJS=credis.o
LOCAL_CFLAGS=-I/usr/local/include/hiredis/
LOCAL_LDFLAGS=-lhiredis
include $(BASE)/build/modmake.rules
