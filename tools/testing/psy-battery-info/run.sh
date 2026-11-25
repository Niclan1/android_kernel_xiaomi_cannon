# extracts the real helpers from power_supply_core.c next to temp_units_test.c, then builds with ASan/UBSan
set -e
K=$(cd "$(dirname "$0")/../../.." && pwd); B=${B:-$PWD}
awk '/^static int power_supply_decicelsius/,/^}/' $K/drivers/power/supply/power_supply_core.c > $B/dc.c
{ awk '/^bool power_supply_battery_info_has_prop\(/,/^}/' $K/drivers/power/supply/power_supply_core.c; awk '/^int power_supply_battery_info_get_prop\(/,/^}/' $K/drivers/power/supply/power_supply_core.c; } > $B/real.c
cp $K/tools/testing/psy-battery-info/temp_units_test.c $B/t.c
${CC:-clang} -g -Wall -fsanitize=address,undefined,signed-integer-overflow -fno-sanitize-recover=all -I $B $B/t.c -o $B/t && $B/t
