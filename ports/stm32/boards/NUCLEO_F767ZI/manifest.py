include("$(PORT_DIR)/boards/manifest.py")
include("$(MPY_DIR)/extmod/pynng/manifest.py")
freeze("$(PORT_DIR)/modules/umsgpack", "umsgpack.py")

