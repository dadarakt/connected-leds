.PHONY: root node root_upload node_upload root_monitor node_monitor monitor clean

root:
	pio run -e mesh_root

node:
	pio run -e mesh_node

root_upload:
	pio run -e mesh_root -t upload

node_upload:
	pio run -e mesh_node -t upload

root_monitor: 
	pio device monitor -e mesh_root

node_monitor: 
	pio device monitor -e mesh_node

clean:
	pio run -t clean
