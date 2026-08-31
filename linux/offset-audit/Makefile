CXX := g++
SDK ?= ../../../../hl2sdk-episode1/hl2sdk-episode1
CXXFLAGS := -m32 -O2 -fPIC -fno-strict-aliasing -D_LINUX -DARCH=i486 -DNDEBUG
CXXFLAGS += -Dstricmp=strcasecmp -D_stricmp=strcasecmp -D_strnicmp=strncasecmp -Dstrnicmp=strncasecmp
CXXFLAGS += -Dstrcmpi=strcasecmp -D_vsnprintf=vsnprintf -D_snprintf=snprintf
CXXFLAGS += -I$(SDK)/public -I$(SDK)/public/tier0 -I$(SDK)/public/tier1
CXXFLAGS += -I$(SDK)/public/dlls -I$(SDK)/public/engine -I$(SDK)/game_shared
LDFLAGS := -m32 -shared

all: insutil_offset_audit_i486.so

insutil_offset_audit_i486.so: offset_audit.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< -L$(SDK)/linux_sdk -l:tier0_i486.so -l:vstdlib_i486.so -ldl

clean:
	rm -f insutil_offset_audit_i486.so

.PHONY: all clean
