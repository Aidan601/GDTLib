#pragma once

#include <string>
#include <vector>

class entity
{
public:
	int id;
	std::string name;
	int gdtSeqNum;
	int parentId;
	int gdfId;
	int gdtId;
	int lineNum;
	int bExport = 0;
	std::string gdfName;

	entity()
    {
        id = -1;
        name = "";
        gdtSeqNum = -1;
        parentId = -1;
        gdfId = -1;
        gdtId = -1;
        lineNum = -1;
        gdfName = "";
	}

	entity(int idInput, std::string nameInput, int gdtSeqNumInput, int parentIdInput, int gdfIdInput, int gdtIdInput, int lineNumInput)
	{
		id = idInput;
		name = nameInput;
		gdtSeqNum = gdtSeqNumInput;
		parentId = parentIdInput;
		gdfId = gdfIdInput;
		gdtId = gdtIdInput;
		lineNum = lineNumInput;
		gdfName = GetGDFName(gdfId);
	}
private:
    std::string GetGDFName(int gdfId)
    {
        static const std::vector<std::string> names = {
            "", // index 0 unused
            "accolade", "accoladelist", "aiassassination", "aifxtable",
            "aimtable", "ainames", "aitype", "attachment", "attachmentcosmeticvariant",
            "attachmentunique", "attackables", "beam", "bonuszmdata", "botsettings",
            "bullet_penetration", "bulletweapon", "cgmediatable", "character",
            "charactercustomizationtable", "characterweaponcustomsettings", "codfuanims",
            "collectible", "collectiblelist", "containers", "customizationcolor",
            "cybercomweapon", "destructiblecharacterdef", "destructibledef",
            "destructiblepiece", "doors", "dualwieldprojectileweapon", "dualwieldweapon",
            "duprenderbundle", "emblem", "entityfximpacts", "entityfxtable",
            "entitysoundimpacts", "flametable", "fog", "footsteptable", "fxcharacterdef",
            "gallery_image", "gallery_imagelist", "gamedifficulty", "gasweapon",
            "gibcharacterdef", "glass", "grenadeweapon", "image", "impactsfxtable",
            "impactsoundstable", "killcam", "killstreak", "laser", "lensflare", "light",
            "lightdescription", "locdmgtable", "maptable", "maptableentry", "material",
            "medal", "medalcase", "medalcaseentry", "medaltable", "meleeweapon",
            "mpbody", "mpdialog", "mpdialog_commander", "mpdialog_player",
            "mpdialog_scorestreak", "mpdialog_taacom", "objective", "objectivelist",
            "physconstraints", "physpreset", "player_character", "playerbodystyle",
            "playerbodytype", "playerfxtable", "playerhead", "playerhelmetstyle",
            "playersoundstable", "postfxbundle", "projectileweapon", "ragdollsettings",
            "rumble", "sanim", "scriptbundle", "sentientevents", "sharedweaponsounds",
            "shellshock", "sitrep", "ssi", "surfacefxtable", "surfacesounddef", "tagfx",
            "teamcolorfx", "tracer", "trainingsimrating", "trainingsimratinglist",
            "turretanims", "turretweapon", "vehicle", "vehiclecustomsettings",
            "vehiclefxdef", "vehicleriders", "vehiclesounddef", "weaponcamo",
            "weaponcamotable", "xanim", "xcam", "xmodel", "xmodelalias", "zbarrier"
        };

        if (gdfId > 0 && gdfId < names.size())
            return names[gdfId];
        else
            return "unknown_gdf";
    }
};