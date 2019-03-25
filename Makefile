CC       = gcc
CFLAGS  := $(shell pkg-config --cflags purple-3 libprotobuf-c) -fPIC -Wno-discarded-qualifiers -Wno-incompatible-pointer-types -Wno-int-conversion -g
LDFLAGS := $(shell pkg-config --libs purple-3 libprotobuf-c)

OBJECTS = mumble-channel.o mumble-channel-tree.o mumble-message.o mumble-protocol.o mumble-user.o mumble.pb-c.o utils.o plugin.o
PLUGIN  = mumble.so

.PHONY: clean install

$(PLUGIN): $(OBJECTS)
	gcc -shared $(LDFLAGS) -o $@ $^

clean:
	rm -f *.o $(PLUGIN)
	rm -f *~
