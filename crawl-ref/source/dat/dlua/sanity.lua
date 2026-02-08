-- Sanity checks that should be run just before the game starts.

local function assert_place_has_map(place)
  if not dgn.map_by_place(place) then
    crawl.mpr("Warning: no map found for " .. place)
  end
end

local function sanity_checks()
  local places = {
    "Zot:$", "Snake:$", "Swamp:$", "Spider:$", "Slime:$", "Elf:$",
    "Vaults:$", "Tomb:$", "Coc:$", "Tar:$", "Dis:$", "Geh:$"
  }
  for _, place in ipairs(places) do
    assert_place_has_map(place)
  end
end

sanity_checks()
