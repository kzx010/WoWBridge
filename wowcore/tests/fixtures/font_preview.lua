-- The same temporary font-only frame is built in the native reference client.
-- This probes text rendering; it does not replace a client getter or font object.
function WBFidelityFontPreview()
  local f=CreateFrame('Frame','WBFontPreview',UIParent)
  f:SetWidth(640) f:SetHeight(360)
  f:SetPoint('TOPLEFT',UIParent,'TOPLEFT',375,-185)
  f:SetFrameStrata('DIALOG')
  local bg=f:CreateTexture(nil,'BACKGROUND')
  bg:SetAllPoints(f) bg:SetTexture(0,0,0,1)
  local n=0
  for col,style in ipairs({{'FRIZQT__.TTF',''},{'FRIZQT__.TTF','OUTLINE'},{'ARIALN.ttf',''},{'ARIALN.ttf','OUTLINE'}}) do
    local title=f:CreateFontString(nil,'OVERLAY')
    title:SetFont('Fonts\\FRIZQT__.TTF',10,'')
    title:SetPoint('TOPLEFT',f,'TOPLEFT',(col-1)*160+8,-5)
    title:SetText(style[1]..' '..style[2])
    for row,height in ipairs({10,12,14,16}) do
      for line,s in ipairs({'0123456789','Dash Elunara'}) do
        n=n+1
        local text=f:CreateFontString('WBFontPreviewText'..n,'OVERLAY')
        text:SetFont('Fonts\\'..style[1],height,style[2])
        text:SetPoint('TOPLEFT',f,'TOPLEFT',(col-1)*160+8,-20-(row-1)*80-(line-1)*28)
        text:SetText(s)
      end
    end
  end
end
