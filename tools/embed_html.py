import json
import pathlib
import sys

html = pathlib.Path(sys.argv[1]).read_text()
logo = pathlib.Path(sys.argv[2]).read_text()
circle = pathlib.Path(sys.argv[3]).read_bytes()
square = pathlib.Path(sys.argv[4]).read_bytes()
save = pathlib.Path(sys.argv[5]).read_bytes()
setting = pathlib.Path(sys.argv[6]).read_bytes()

parts = []
parts.append("static const char local_page[] = " + json.dumps(html) + ";\n")
parts.append("static const char local_logo[] = " + json.dumps(logo) + ";\n")

circle_hex = ", ".join("0x%02x" % byte for byte in circle)
parts.append("static const unsigned char local_circle[] = { %s };\n" % circle_hex)
parts.append("static const size_t local_circle_size = sizeof(local_circle);\n")

square_hex = ", ".join("0x%02x" % byte for byte in square)
parts.append("static const unsigned char local_square[] = { %s };\n" % square_hex)
parts.append("static const size_t local_square_size = sizeof(local_square);\n")

save_hex = ", ".join("0x%02x" % byte for byte in save)
parts.append("static const unsigned char local_save[] = { %s };\n" % save_hex)
parts.append("static const size_t local_save_size = sizeof(local_save);\n")

setting_hex = ", ".join("0x%02x" % byte for byte in setting)
parts.append("static const unsigned char local_setting[] = { %s };\n" % setting_hex)
parts.append("static const size_t local_setting_size = sizeof(local_setting);\n")

pathlib.Path(sys.argv[7]).write_text("".join(parts))
