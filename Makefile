INCFLAGS = -I. -I/home/yntp16/Downloads/cJSON  -I/home/yntp16/wuhan2020/webrtc
LDFLAGS = -Llibns -lns -lpthread -lm -lcjson -lopus -lwebrtc 
DBGFLAGS = -DZDEBUG  -g
all:
	gcc zns.c $(INCFLAGS) $(LDFLAGS) $(DBGFLAGS) -o zns
