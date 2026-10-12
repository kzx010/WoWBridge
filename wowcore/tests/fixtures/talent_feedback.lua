-- Live service values at level 60, Improved Mark 5 + Nature's Focus 5 + Insect Swarm 1.
-- Rank-seven Mark: int(285/12/20 * 1.35), before native carrier conversion.
function __WBMarkTooltip(improved)
  return {effects=improved and {{384,384},{16,16},{27,27}} or {{285,285},{12,12},{20,20}},
          duration_ms=1800000,cast_ms=0,range_max=30,cooldown_ms=0}
end

function __WBFeralFeedbackFixture()
  __WBTalentFixture(false)
  local reply = __WB.talents
  reply.talents, reply.points_spent, reply.points_available = {}, 46, 5
  local ids, rows = {9850,9830,8983}, {}
  for _, t in ipairs(reply.trees[2].talents) do
    t.rank, t.learnable, t.meets_prerequisites = t.max_rank, false, true
    reply.talents[tostring(t.id)] = t.rank
    local id = t.rank_spells[t.rank]
    table.insert(ids,id)
    table.insert(rows,{spell=id,skill_line=134,tab=2,passive=t.id~=804 and t.id~=1162})
  end
  reply.trees[2].points_spent = 46
  __WBTalents(reply)
  for _, id in ipairs({9850,9830,8983}) do table.insert(rows,{spell=id,skill_line=134,tab=2,passive=false}) end
  __WBClassSpells({class='druid',class_id=11,known_spells=ids,spells=rows,unlearned_spells={},
    lines={['134']={name='Feral Combat',tab=2}},
    spell_tooltips={['9850']={effects={{138,138},{1,1},{0,0}}},['8983']={duration_ms=5000}}})
  __WB.resolve('player').skyrim = {}
  __WB.skyrimCosts = {[9850]=40,[9830]=48,[9881]=10,[16979]=5,[16857]=0}
  __WB.actions[2],__WB.actions[3],__WB.actions[4] = {spell=9850},{spell=8983},{spell=9830}
  for i=2,4 do __WBFire('ACTIONBAR_SLOT_CHANGED',i) end
end

function __WBTalentFeedbackFixture()
  __WBTalentFixture(false)
  local reply = __WB.talents
  reply.talents = {['821']=5,['823']=5,['827']=1}
  reply.points_spent, reply.points_available = 11, 40
  reply.trees[3].points_spent = 11
  for _, t in ipairs(reply.trees[3].talents) do
    t.rank = reply.talents[tostring(t.id)] or 0
    t.learnable = t.rank < t.max_rank and t.tier <= 2 and t.meets_prerequisites
  end
  __WBTalents(reply)
  __WBClassSpells({class='druid',class_id=11,
    lines={['573']={name='Restoration',tab=3}},
    known_spells={9885,17055,17068,5570},unlearned_spells={},
    spells={{spell=9885,skill_line=573,tab=3,passive=false},
            {spell=17055,skill_line=573,tab=3,passive=true},
            {spell=17068,skill_line=573,tab=3,passive=true},
            {spell=5570,skill_line=573,tab=3,passive=false}},
    spell_tooltips={['9885']=__WBMarkTooltip(true)}})
  __WB.actions[2] = {spell=9885}
  __WBFire('ACTIONBAR_SLOT_CHANGED',2)
end
