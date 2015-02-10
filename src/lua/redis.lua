package.path = package.path .. ";lua/?.lua;../?.lua"

print ("Script Init Begin")

local pool = require("pool")

function parse(body)
   local lines = body:strip():split("\n")
   table.remove(lines, 1)

   local configs = {}
   -- parse
   for i,line in ipairs(lines) do
      local xs = line:split(" ")

      local addr = xs[4]:split(":")
      ip, port = addr[1], addr[2]

      local role = "master"
      if string.find(xs[5], "master") == nil then
         role = "slave"
      end

      local c = {
         id = xs[3],
         addr = xs[4],
         ip = ip,
         port = tonumber(port),
         role = role,
         master_id = xs[6],
         status = xs[10],
      }

      if role == "master" then
         local slots = {}
         for i = 11, #xs do
            local range = xs[i]
            local slot = {}
            local pair = range:split("-")

            slot.left = tonumber(pair[1])
            slot.right = tonumber(pair[2])
            table.insert(slots, slot)
         end
         c.ranges = slots
      end

      table.insert(configs, c)
   end
   return configs
end

function update_cluster_nodes(msg)

   -- parse message returned by 'cluster nodes'
   local configs, err = parse(msg)
   
   if err then
      return 1
   end

   -- reconstruct servers, fix adds and drops
   pool:set_servers(configs)

   -- rebuild replica sets
   pool:build_replica_sets()

   -- bind replica sets to slots
   pool:bind_slots()

   -- 0 is success
   return 0
end

print ("Script Init Done")