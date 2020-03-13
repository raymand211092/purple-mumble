CC       = gcc
CFLAGS  := $(shell pkg-config --cflags purple-3) -fPIC -Wno-discarded-qualifiers -Wno-incompatible-pointer-types -Wno-int-conversion -g
LDFLAGS := $(shell pkg-config --libs purple-3)

OBJECTS = mumble-channel.o mumble-channel-tree.o mumble-input-stream.o mumble-message.o mumble-output-stream.o mumble-protocol.o mumble-user.o plugin.o protobuf-utils.o utils.o
PLUGIN  = mumble.so

.PHONY: clean

$(PLUGIN): $(OBJECTS)
	$(CC) -shared $(LDFLAGS) -o $@ $^

clean:
	rm -f *.o $(PLUGIN)
	rm -f *~
