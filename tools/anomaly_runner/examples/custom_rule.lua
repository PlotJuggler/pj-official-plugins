-- A CUSTOM detector (not one of the built-ins): flag every sample that falls
-- outside a [LO, HI] comfort window as a warning point. Shows that the runner
-- runs arbitrary Luau you author yourself — pass it with:
--   --script tools/anomaly_runner/examples/custom_rule.lua --source anomaly_demo/value
local s = series("--SOURCE--")
local LO, HI = -1.0, 2.0
for i = 0, s:size() - 1 do
  local p = s:at(i)
  if p.v < LO or p.v > HI then
    createEvent(p.t, p.v, {label = "out-of-window", severity = "warning", color = "#e08020"})
  end
end
