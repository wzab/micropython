include("$(PORT_DIR)/boards/manifest.py")
freeze("$(PORT_DIR)/modules/umsgpack", "umsgpack.py")
freeze("$(PORT_DIR)/modules/pynng")

