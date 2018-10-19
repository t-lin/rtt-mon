LIBTINS = $(HOME)/libtins
MAILIO = $(HOME)/mailio
CPPFLAGS += -I$(LIBTINS)/include -I$(MAILIO)/include
LDFLAGS += -L$(LIBTINS)/lib -L$(MAILIO)/lib/ -ltins -lpcap -lmailio -lboost_system -lssl -lcrypto -lpthread -ldl -lboost_random -lboost_regex
CXXFLAGS += -std=c++14 -O3 -Wall

BINNAME = rtt-mon

all:  main.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) main.cpp -o $(BINNAME) $(LDFLAGS)

debug:  main.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -g main.cpp -o $(BINNAME) $(LDFLAGS)

static: main.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -static main.cpp -o $(BINNAME) $(LDFLAGS)

clean:
	rm -f $(BINNAME)

