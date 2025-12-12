# 2025-12-12T11:39:49.577864
import vitis

client = vitis.create_client()
client.set_workspace(path="DigFInal")

platform = client.get_component(name="Digfinal")
status = platform.build()

comp = client.get_component(name="Final")
comp.build()

status = platform.build()

comp.build()

