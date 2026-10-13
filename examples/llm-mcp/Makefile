TOP = ../..
TCCBIN = $(TOP)/tcc

all: tcc-mcp-server

# The server is compiled by TCC itself, together with libtcc.c
# (the unity build of the whole compiler), so there is no runtime
# dependency on a system compiler or interpreter.
tcc-mcp-server: server.c $(TCCBIN) $(TOP)/libtcc.c $(TOP)/libtcc1.a
	$(TCCBIN) -B$(TOP) -I$(TOP) -o $@ server.c $(TOP)/libtcc.c -lm -ldl -lpthread

$(TCCBIN) $(TOP)/libtcc1.a:
	$(MAKE) -C $(TOP)

test: tcc-mcp-server
	python3 -m unittest -v test_mcp

clean:
	rm -f tcc-mcp-server

.PHONY: all test clean
