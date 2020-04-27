INCFLAGS = -I. -I/home/yntp16/Downloads/cJSON
LDFLAGS = -Llibns -lns -lpthread -lm -lcjson -lopus
DBGFLAGS = -DZDEBUG  -g
all:
	gcc zns.c $(INCFLAGS) $(LDFLAGS) $(DBGFLAGS) -o zns
