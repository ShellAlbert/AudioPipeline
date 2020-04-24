INCFLAGS = -I. -I/home/yntp16/Downloads/cJSON
LDFLAGS = -Llibns -lns -lpthread -lm -lcjson -lopus
all:
	gcc zns.c $(INCFLAGS) $(LDFLAGS) -o zns
