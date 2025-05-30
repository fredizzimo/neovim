local M = {}

---@class vim.ui.img.Image
---@field id integer

---@class vim.ui.img.SizedImage
---@field img vim.ui.Image 
---@field num_cols integer
---@field num_rows integer
---@field keep_aspect? boolean

---@class vim.ui.img.PlacementOpts
---@field start_col integer
---@field start_row integer
---@field num_cols integer
---@field num_rows integer
---@field highlight? string[] | string


---@type table<integer, vim.ui.Image>
local id_to_image ={}
setmetatable(id_to_image, { __mode = "v" })
M._id_to_image = id_to_image

local next_id = 1

---@return vim.ui.Image
function M.create(data, opts)
  local ret = {
    id = next_id
  }
  local internal = {
    data = data,
    sent = false
  }
  setmetatable(ret, internal)
  id_to_image[next_id] = ret
  next_id = next_id + 1
  return ret
end


---@param img vim.ui.img.SizedImage
---@param opts vim.ui.img.PlacementOpts
function M.create_fragments(img, opts)
  local ret = {}
  for i = 1, opts.num_rows do
    local row = { {
      img_id = img.img.id,
      img_width = img.num_cols,
      img_height = img.num_rows,
      keep_aspect = img.keep_aspect,
      start_col = opts.start_col,
      start_row = opts.start_row + i,
      num_cols = opts.num_cols,
    }, opts.highlight }
    if opts.num_rows == 1 then
      table.insert(ret, row)
    else
      table.insert(ret, {row})
    end

  end

  -- reference the image data, so that it's not garbage collected until used
  setmetatable(ret, img.img)
  return ret
end

return M
