from pathlib import Path
import sys

p = Path(__file__).resolve().parents[2] / "firmware/Nexstar_Protocol_Converter/Nexstar_Protocol_Converter.ino"
s = p.read_text(encoding="utf-8")
required = ["refreshProtocolDash", "updateNow", "catPopulate", "gotoRaDec", "gotoAltAz", "csHost"]
missing = [x for x in required if x not in s]
forbidden = ["pollPreset", "pollSelectChanged", "Mount poll preset"]
present = [x for x in forbidden if x in s]
if missing or present:
    print("Missing:", missing)
    print("Forbidden still present:", present)
    sys.exit(1)
print("Browser static checks passed")
