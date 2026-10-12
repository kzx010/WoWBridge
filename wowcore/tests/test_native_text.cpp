#include <catch_amalgamated.hpp>
#include "../../skse/src/NativeText.h"

TEST_CASE("native activation presentation leaves readable text and preserves unknown tokens", "[native-text]")
{
    CHECK(NativeText::Activation("Search <img src='DiamondMarker'>Barrel") == "Search Barrel");
    CHECK(NativeText::Activation("<font color='#FFFFFF'>Steal</font><br/>Owned Barrel") == "Steal\nOwned Barrel");
    CHECK(NativeText::Activation("<IMG src=\"a>b\"/>Use &amp; take &lt;2&gt;") == "Use & take <2>");
    CHECK(NativeText::Activation("Restore <2> points. <unknown>Keep</unknown>") == "Restore <2> points. <unknown>Keep</unknown>");
    CHECK(NativeText::Activation("<imginary>plain</imginary>") == "<imginary>plain</imginary>");
    CHECK(NativeText::Activation("<img src='unfinished") == "<img src='unfinished");
    CHECK(NativeText::Activation("<font color='red'>Quest &quot;Item&quot;</font>") == "Quest \"Item\"");
}

TEST_CASE("expanded native Survival clauses follow the game's enabled state", "[native-text]")
{
    const auto wine="Restore 15 Stamina. [SURV=Restore 2 points of Hunger.]";
    CHECK(NativeText::ItemDescription(wine,false)=="Restore 15 Stamina.");
    CHECK(NativeText::ItemDescription(wine,true)=="Restore 15 Stamina. Restore 2 points of Hunger.");
    CHECK(NativeText::ItemDescription("[SURV=Food.] [SURV=Warmth.]",true)=="Food. Warmth.");
    CHECK(NativeText::ItemDescription("Unknown [OTHER=value] <2>",false)=="Unknown [OTHER=value] <2>");
    CHECK(NativeText::ItemDescription("[SURV=Unclosed",false)=="[SURV=Unclosed");
}
