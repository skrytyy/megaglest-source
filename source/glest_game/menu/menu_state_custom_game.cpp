//	This file is part of Glest (www.glest.org)
//
//	Copyright (C) 2001-2005 Martiño Figueroa
//
//	You can redistribute this code and/or modify it under
//	the terms of the GNU General Public License as published
//	by the Free Software Foundation; either version 2 of the
//	License, or (at your option) any later version
// ==============================================================
#include "menu_state_custom_game.h"

#include "renderer.h"
#include "sound_renderer.h"
#include "core_data.h"
#include "config.h"
#include "menu_state_new_game.h"
#include "menu_state_masterserver.h"
#include "menu_state_join_game.h"
#include "metrics.h"
#include "network_manager.h"
#include "network_message.h"
#include "client_interface.h"
#include "conversion.h"
#include "socket.h"
#include "game.h"
#include "util.h"
#include <algorithm>
#include <time.h>
#include <curl/curl.h>
#include "cache_manager.h"
#include <iterator>
#include "map_preview.h"
#include "gen_uuid.h"
#include "megaglest_cegui_manager.h"

#include "leak_dumper.h"

namespace Glest{ namespace Game{

using namespace ::Shared::Util;

const int MASTERSERVER_BROADCAST_MAX_WAIT_RESPONSE_SECONDS  = 15;
const int MASTERSERVER_BROADCAST_PUBLISH_SECONDS   			= 6;
const int BROADCAST_MAP_DELAY_SECONDS 						= 5;
const int BROADCAST_SETTINGS_SECONDS  						= 4;
static const char *SAVED_GAME_FILENAME 				= "lastCustomGameSettings.mgg";
static const char *DEFAULT_GAME_FILENAME 			= "data/defaultGameSetup.mgg";
static const char *DEFAULT_NETWORKGAME_FILENAME 	= "data/defaultNetworkGameSetup.mgg";

const int mapPreviewTexture_X = 5;
const int mapPreviewTexture_Y = 185;
const int mapPreviewTexture_W = 150;
const int mapPreviewTexture_H = 150;

struct FormatString {
	void operator()(string &s) {
		s = formatString(s);
	}
};

// =====================================================
// 	class MenuStateCustomGame
// =====================================================
enum THREAD_NOTIFIER_TYPE {
	tnt_MASTERSERVER = 1,
	tnt_CLIENTS = 2
};

MenuStateCustomGame::MenuStateCustomGame(Program *program, MainMenu *mainMenu,
		bool openNetworkSlots,ParentMenuState parentMenuState, bool autostart,
		GameSettings *settings, bool masterserverMode,
		string autoloadScenarioName) :
		MenuState(program, mainMenu, "new-game") {
	try {

	this->headlessServerMode = masterserverMode;
	if(this->headlessServerMode == true) {
		printf("Waiting for players to join and start a game...\n");
	}

	this->gameUUID = getUUIDAsString();

	this->zoomedMap=false;
	this->render_mapPreviewTexture_X = mapPreviewTexture_X;
	this->render_mapPreviewTexture_Y = mapPreviewTexture_Y;
	this->render_mapPreviewTexture_W = mapPreviewTexture_W;
	this->render_mapPreviewTexture_H = mapPreviewTexture_H;

	this->lastMasterServerSettingsUpdateCount = 0;
	this->masterserverModeMinimalResources = true;
	this->parentMenuState=parentMenuState;
	this->factionVideo = NULL;
	factionVideoSwitchedOffVolume=false;

	//printf("this->masterserverMode = %d [%d]\n",this->masterserverMode,masterserverMode);

	forceWaitForShutdown = true;
	this->autostart = autostart;
	this->autoStartSettings = settings;

	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d] autostart = %d\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,autostart);

	containerName = "CustomGame";
	//activeInputLabel=NULL;
	showGeneralError = false;
	generalErrorToShow = "---";
	currentFactionLogo = "";
	factionTexture=NULL;
	currentTechName_factionPreview="";
	currentFactionName_factionPreview="";
	mapPreviewTexture=NULL;
	hasCheckedForUPNP = false;
	needToPublishDelayed=false;
	mapPublishingDelayTimer=time(NULL);

    lastCheckedCRCTilesetName					= "";
    lastCheckedCRCTechtreeName					= "";
    lastCheckedCRCMapName						= "";

    last_Forced_CheckedCRCTilesetName			= "";
    last_Forced_CheckedCRCTechtreeName			= "";
    last_Forced_CheckedCRCMapName				= "";

    lastCheckedCRCTilesetValue					= 0;
    lastCheckedCRCTechtreeValue					= 0;
    lastCheckedCRCMapValue						= 0;

	publishToMasterserverThread = NULL;
	publishToClientsThread = NULL;

	Lang &lang= Lang::getInstance();
	NetworkManager &networkManager= NetworkManager::getInstance();
    Config &config = Config::getInstance();
    defaultPlayerName = config.getString("NetPlayerName",Socket::getHostName().c_str());
    enableFactionTexturePreview = config.getBool("FactionPreview","true");
    enableMapPreview = config.getBool("MapPreview","true");

    showFullConsole=false;

	enableScenarioTexturePreview = Config::getInstance().getBool("EnableScenarioTexturePreview","true");
	scenarioLogoTexture=NULL;
	previewLoadDelayTimer=time(NULL);
	needToLoadTextures=true;
	this->autoloadScenarioName = autoloadScenarioName;
	this->dirList = Config::getInstance().getPathListForType(ptScenarios);

    //mainMessageBox.registerGraphicComponent(containerName,"mainMessageBox");
	//mainMessageBox.init(lang.getString("Ok"));
	//mainMessageBox.setEnabled(false);
	mainMessageBoxState=0;

	//initialize network interface
	NetworkManager::getInstance().end();

	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	serverInitError = false;
	try {
		networkManager.init(nrServer,openNetworkSlots);
	}
	catch(const std::exception &ex) {
		serverInitError = true;
		char szBuf[8096]="";
		snprintf(szBuf,8096,"In [%s::%s %d]\nNetwork init error:\n%s\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,ex.what());
		SystemFlags::OutputDebug(SystemFlags::debugError,szBuf);
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s",szBuf);

		showGeneralError=true;
		generalErrorToShow = szBuf;
	}

	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	needToSetChangedGameSettings = false;
	needToRepublishToMasterserver = false;
	needToBroadcastServerSettings = false;
	showMasterserverError = false;
	tMasterserverErrorElapsed = 0;
	masterServererErrorToShow = "---";
	lastSetChangedGameSettings  = 0;
	lastMasterserverPublishing 	= 0;
	lastNetworkPing				= 0;
	soundConnectionCount=0;

	techTree.reset(new TechTree(config.getPathListForType(ptTechs)));

	setupCEGUIWidgets(openNetworkSlots);

	//int labelOffset=23;
	//int setupPos=605;
	//int mapHeadPos=330;
	//int mapPos=mapHeadPos-labelOffset;
	//int aHeadPos=240;
	//int aPos=aHeadPos-labelOffset;
	//int networkHeadPos=700;
	//int networkPos=networkHeadPos-labelOffset;
	int xoffset=10;

	//create
	//int buttonx=200;
	//int buttony=180;
	//buttonReturn.registerGraphicComponent(containerName,"buttonReturn");
	//buttonReturn.init(buttonx, buttony, 125);

	//buttonRestoreLastSettings.registerGraphicComponent(containerName,"buttonRestoreLastSettings");
	//buttonRestoreLastSettings.init(buttonx+130, buttony, 220);

	//buttonPlayNow.registerGraphicComponent(containerName,"buttonPlayNow");
	//buttonPlayNow.init(buttonx+130+225, buttony, 125);

	//labelLocalGameVersion.registerGraphicComponent(containerName,"labelLocalGameVersion");
	//labelLocalGameVersion.init(10, networkHeadPos+labelOffset);

	//labelLocalIP.registerGraphicComponent(containerName,"labelLocalIP");
	//labelLocalIP.init(360, networkHeadPos+labelOffset);

	string ipText = "none";
	std::vector<std::string> ipList = Socket::getLocalIPAddressList();
	if(ipList.empty() == false) {
		ipText = "";
		for(int idx = 0; idx < (int)ipList.size(); idx++) {
			string ip = ipList[idx];
			if(ipText != "") {
				ipText += ", ";
			}
			ipText += ip;
		}
	}
	string serverPort=config.getString("PortServer", intToStr(GameConstants::serverPort).c_str());
	string externalPort=config.getString("PortExternal", serverPort.c_str());
	//labelLocalIP.setText(lang.getString("LanIP") + ipText + "  ( "+serverPort+" / "+externalPort+" )");
	ServerSocket::setExternalPort(strToInt(externalPort));

//	if(EndsWith(glestVersionString, "-dev") == false){
//		labelLocalGameVersion.setText(glestVersionString);
//	}
//	else {
//		labelLocalGameVersion.setText(glestVersionString + " [" + getCompileDateTime() + ", " + getGITRevisionString() + "]");
//	}

	xoffset=70;
	// MapFilter
	//labelMapFilter.registerGraphicComponent(containerName,"labelMapFilter");
	//labelMapFilter.init(xoffset+310, mapHeadPos);
	//labelMapFilter.setText(lang.getString("MapFilter")+":");

	//listBoxMapFilter.registerGraphicComponent(containerName,"listBoxMapFilter");
	//listBoxMapFilter.init(xoffset+310, mapPos, 80);
	//listBoxMapFilter.pushBackItem("-");
	//for(int i=1; i<GameConstants::maxPlayers+1; ++i){
	//	listBoxMapFilter.pushBackItem(intToStr(i));
	//}
	//listBoxMapFilter.setSelectedItemIndex(0);

	// Map
	//labelMap.registerGraphicComponent(containerName,"labelMap");
	//labelMap.init(xoffset+100, mapHeadPos);
	//labelMap.setText(lang.getString("Map")+":");

	//map listBox
	//listBoxMap.registerGraphicComponent(containerName,"listBoxMap");
	//listBoxMap.init(xoffset+100, mapPos, 200);
	// put them all in a set, to weed out duplicates (gbm & mgm with same name)
	// will also ensure they are alphabetically listed (rather than how the OS provides them)
	//int initialMapSelection = setupMapList("");
    //listBoxMap.setItems(formattedPlayerSortedMaps[0]);
    //listBoxMap.setSelectedItemIndex(initialMapSelection);

    //labelMapInfo.registerGraphicComponent(containerName,"labelMapInfo");
	//labelMapInfo.init(xoffset+100, mapPos-labelOffset-10, 200, 40);

//    labelTileset.registerGraphicComponent(containerName,"labelTileset");
//	labelTileset.init(xoffset+460, mapHeadPos);
//	labelTileset.setText(lang.getString("Tileset"));

	//tileset listBox
	//listBoxTileset.registerGraphicComponent(containerName,"listBoxTileset");
	//listBoxTileset.init(xoffset+460, mapPos, 150);

	//setupTilesetList("");
	//Chrono seed(true);
	//srand((unsigned int)seed.getCurTicks());

	//listBoxTileset.setSelectedItemIndex(rand() % listBoxTileset.getItemCount());

    //tech Tree listBox
    //int initialTechSelection = setupTechList("", true);

	//listBoxTechTree.registerGraphicComponent(containerName,"listBoxTechTree");
	//listBoxTechTree.init(xoffset+650, mapPos, 150);
	//if(listBoxTechTree.getItemCount() > 0) {
	//	listBoxTechTree.setSelectedItemIndex(initialTechSelection);
	//}

    //labelTechTree.registerGraphicComponent(containerName,"labelTechTree");
	//labelTechTree.init(xoffset+650, mapHeadPos);
	//labelTechTree.setText(lang.getString("TechTree"));

	// fog - o - war
	// @350 ? 300 ?
//	labelFogOfWar.registerGraphicComponent(containerName,"labelFogOfWar");
//	labelFogOfWar.init(xoffset+100, aHeadPos, 130);
//	labelFogOfWar.setText(lang.getString("FogOfWar"));

//	listBoxFogOfWar.registerGraphicComponent(containerName,"listBoxFogOfWar");
//	listBoxFogOfWar.init(xoffset+100, aPos, 130);
//	listBoxFogOfWar.pushBackItem(lang.getString("Enabled"));
//	listBoxFogOfWar.pushBackItem(lang.getString("Explored"));
//	listBoxFogOfWar.pushBackItem(lang.getString("Disabled"));
//	listBoxFogOfWar.setSelectedItemIndex(0);

	// Allow Observers
//	labelAllowObservers.registerGraphicComponent(containerName,"labelAllowObservers");
//	labelAllowObservers.init(xoffset+310, aHeadPos, 80);
//	labelAllowObservers.setText(lang.getString("AllowObservers"));
//
//	checkBoxAllowObservers.registerGraphicComponent(containerName,"checkBoxAllowObservers");
//	checkBoxAllowObservers.init(xoffset+310, aPos);
//	checkBoxAllowObservers.setValue(false);

	vector<string> rMultiplier;
	for(int i=0; i<45; ++i){
		rMultiplier.push_back(floatToStr(0.5f+0.1f*i,1));
	}

//	labelFallbackCpuMultiplier.registerGraphicComponent(containerName,"labelFallbackCpuMultiplier");
//	labelFallbackCpuMultiplier.init(xoffset+460, aHeadPos, 80);
//	labelFallbackCpuMultiplier.setText(lang.getString("FallbackCpuMultiplier"));
//
//	listBoxFallbackCpuMultiplier.registerGraphicComponent(containerName,"listBoxFallbackCpuMultiplier");
//	listBoxFallbackCpuMultiplier.init(xoffset+460, aPos, 80);
//	listBoxFallbackCpuMultiplier.setItems(rMultiplier);
//	listBoxFallbackCpuMultiplier.setSelectedItem("1.0");

	// Allow Switch Team Mode
	//labelEnableSwitchTeamMode.registerGraphicComponent(containerName,"labelEnableSwitchTeamMode");
	//labelEnableSwitchTeamMode.init(xoffset+310, aHeadPos+45, 80);
	//labelEnableSwitchTeamMode.setText(lang.getString("EnableSwitchTeamMode"));

	//checkBoxEnableSwitchTeamMode.registerGraphicComponent(containerName,"checkBoxEnableSwitchTeamMode");
	//checkBoxEnableSwitchTeamMode.init(xoffset+310, aPos+45);
	//checkBoxEnableSwitchTeamMode.setValue(false);

	//labelAISwitchTeamAcceptPercent.registerGraphicComponent(containerName,"labelAISwitchTeamAcceptPercent");
	//labelAISwitchTeamAcceptPercent.init(xoffset+460, aHeadPos+45, 80);
	//labelAISwitchTeamAcceptPercent.setText(lang.getString("AISwitchTeamAcceptPercent"));

	//listBoxAISwitchTeamAcceptPercent.registerGraphicComponent(containerName,"listBoxAISwitchTeamAcceptPercent");
//	listBoxAISwitchTeamAcceptPercent.init(xoffset+460, aPos+45, 80);
//	for(int i = 0; i <= 100; i = i + 10) {
//		listBoxAISwitchTeamAcceptPercent.pushBackItem(intToStr(i));
//	}
//	listBoxAISwitchTeamAcceptPercent.setSelectedItem(intToStr(30));

//	labelAllowNativeLanguageTechtree.registerGraphicComponent(containerName,"labelAllowNativeLanguageTechtree");
//	labelAllowNativeLanguageTechtree.init(xoffset+650, mapHeadPos-50);
//	labelAllowNativeLanguageTechtree.setText(lang.getString("AllowNativeLanguageTechtree"));
//
//	checkBoxAllowNativeLanguageTechtree.registerGraphicComponent(containerName,"checkBoxAllowNativeLanguageTechtree");
//	checkBoxAllowNativeLanguageTechtree.init(xoffset+650, mapHeadPos-70);
//	checkBoxAllowNativeLanguageTechtree.setValue(false);

    // player status
//	listBoxPlayerStatus.registerGraphicComponent(containerName,"listBoxPlayerStatus");
//	listBoxPlayerStatus.init(810, buttony, 150);
//	vector<string> playerStatuses;
//	playerStatuses.push_back(lang.getString("PlayerStatusSetup"));
//	playerStatuses.push_back(lang.getString("PlayerStatusBeRightBack"));
//	playerStatuses.push_back(lang.getString("PlayerStatusReady"));
//	listBoxPlayerStatus.setItems(playerStatuses);
//	listBoxPlayerStatus.setSelectedItemIndex(2,true);
//	listBoxPlayerStatus.setTextColor(Vec3f(0.0f,1.0f,0.0f));
//	listBoxPlayerStatus.setLighted(false);
//	listBoxPlayerStatus.setVisible(true);

	// Network Scenario
	//int scenarioX=810;
	//int scenarioY=140;
    //labelScenario.registerGraphicComponent(containerName,"labelScenario");
    //labelScenario.init(scenarioX, scenarioY);
    //labelScenario.setText(lang.getString("Scenario"));
	//listBoxScenario.registerGraphicComponent(containerName,"listBoxScenario");
    //listBoxScenario.init(scenarioX, scenarioY-30,190);
    //checkBoxScenario.registerGraphicComponent(containerName,"checkBoxScenario");
    //checkBoxScenario.init(scenarioX+90, scenarioY);
    //checkBoxScenario.setValue(false);

    //scenario listbox
//    vector<string> resultsScenarios;
//	findDirs(dirList, resultsScenarios);
//	// Filter out only scenarios with no network slots
//	for(int i= 0; i < (int)resultsScenarios.size(); ++i) {
//		string scenario = resultsScenarios[i];
//		string file = Scenario::getScenarioPath(dirList, scenario);
//
//		try {
//			if(file != "") {
//				bool isTutorial = Scenario::isGameTutorial(file);
//				Scenario::loadScenarioInfo(file, &scenarioInfo, isTutorial);
//
//				bool isNetworkScenario = false;
//				for(unsigned int j = 0; isNetworkScenario == false && j < (unsigned int)GameConstants::maxPlayers; ++j) {
//					if(scenarioInfo.factionControls[j] == ctNetwork) {
//						isNetworkScenario = true;
//					}
//				}
//				if(isNetworkScenario == true) {
//					scenarioFiles.push_back(scenario);
//				}
//			}
//		}
//		catch(const std::exception &ex) {
//		    char szBuf[8096]="";
//		    snprintf(szBuf,8096,"In [%s::%s %d]\nError loading scenario [%s]:\n%s\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,scenario.c_str(),ex.what());
//		    SystemFlags::OutputDebug(SystemFlags::debugError,szBuf);
//		    if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s",szBuf);
//
//		    showGeneralError=true;
//			generalErrorToShow = szBuf;
//		    //throw megaglest_runtime_error(szBuf);
//		}
//	}
//	resultsScenarios.clear();
//	for(int i = 0; i < (int)scenarioFiles.size(); ++i) {
//		resultsScenarios.push_back(formatString(scenarioFiles[i]));
//	}
//    listBoxScenario.setItems(resultsScenarios);
//    if(resultsScenarios.empty() == true) {
//    	checkBoxScenario.setEnabled(false);
//    }

	// Advanced Options
//	labelAdvanced.registerGraphicComponent(containerName,"labelAdvanced");
//	labelAdvanced.init(810, 80, 80);
//	labelAdvanced.setText(lang.getString("AdvancedGameOptions"));
//
//	checkBoxAdvanced.registerGraphicComponent(containerName,"checkBoxAdvanced");
//	checkBoxAdvanced.init(810,  80-labelOffset);
//	checkBoxAdvanced.setValue(false);

	// network things
	// PublishServer
	xoffset=90;

//	labelPublishServer.registerGraphicComponent(containerName,"labelPublishServer");
//	labelPublishServer.init(50, networkHeadPos, 100);
//	labelPublishServer.setText(lang.getString("PublishServer"));

//	checkBoxPublishServer.registerGraphicComponent(containerName,"checkBoxPublishServer");
//	checkBoxPublishServer.init(50, networkPos);
//
//	checkBoxPublishServer.setValue(false);
//	if((this->headlessServerMode == true ||
//		(openNetworkSlots == true && parentMenuState != pLanGame)) &&
//			GlobalStaticFlags::isFlagSet(gsft_lan_mode) == false) {
//		checkBoxPublishServer.setValue(true);
//	}

//	labelGameName.registerGraphicComponent(containerName,"labelGameName");
//	labelGameName.init(50+checkBoxPublishServer.getW()+2, networkPos,200);
//	labelGameName.setFont(CoreData::getInstance().getMenuFontBig());
//	labelGameName.setFont3D(CoreData::getInstance().getMenuFontBig3D());
//	if(this->headlessServerMode == false) {
//		labelGameName.setText(defaultPlayerName+"'s game");
//	}
//	else {
//		labelGameName.setText("headless ("+defaultPlayerName+")");
//	}
//	labelGameName.setEditable(true);
//	labelGameName.setMaxEditWidth(20);
//	labelGameName.setMaxEditRenderWidth(200);


	//bool allowInProgressJoin = Config::getInstance().getBool("EnableJoinInProgressGame","false");
//	labelAllowInGameJoinPlayer.registerGraphicComponent(containerName,"labelAllowInGameJoinPlayer");
//	labelAllowInGameJoinPlayer.init(xoffset+410, 670, 80);
//	labelAllowInGameJoinPlayer.setText(lang.getString("AllowInGameJoinPlayer"));
//	labelAllowInGameJoinPlayer.setVisible(allowInProgressJoin);
//
//	checkBoxAllowInGameJoinPlayer.registerGraphicComponent(containerName,"checkBoxAllowInGameJoinPlayer");
//	checkBoxAllowInGameJoinPlayer.init(xoffset+600, 670);
//	checkBoxAllowInGameJoinPlayer.setValue(false);
//	checkBoxAllowInGameJoinPlayer.setVisible(allowInProgressJoin);


//	labelAllowTeamUnitSharing.registerGraphicComponent(containerName,"labelAllowTeamUnitSharing");
//	labelAllowTeamUnitSharing.init(xoffset+410, 670, 80);
//	labelAllowTeamUnitSharing.setText(lang.getString("AllowTeamUnitSharing"));
//	labelAllowTeamUnitSharing.setVisible(true);
//
//	checkBoxAllowTeamUnitSharing.registerGraphicComponent(containerName,"checkBoxAllowTeamUnitSharing");
//	checkBoxAllowTeamUnitSharing.init(xoffset+600, 670);
//	checkBoxAllowTeamUnitSharing.setValue(false);
//	checkBoxAllowTeamUnitSharing.setVisible(true);

//	labelAllowTeamResourceSharing.registerGraphicComponent(containerName,"labelAllowTeamResourceSharing");
//	labelAllowTeamResourceSharing.init(xoffset+410, 640, 80);
//	labelAllowTeamResourceSharing.setText(lang.getString("AllowTeamResourceSharing"));
//	labelAllowTeamResourceSharing.setVisible(true);
//
//	checkBoxAllowTeamResourceSharing.registerGraphicComponent(containerName,"checkBoxAllowTeamResourceSharing");
//	checkBoxAllowTeamResourceSharing.init(xoffset+600, 640);
//	checkBoxAllowTeamResourceSharing.setValue(false);
//	checkBoxAllowTeamResourceSharing.setVisible(true);


	// Network Pause for lagged clients
	//labelNetworkPauseGameForLaggedClients.registerGraphicComponent(containerName,"labelNetworkPauseGameForLaggedClients");
	//labelNetworkPauseGameForLaggedClients.init(labelAllowInGameJoinPlayer.getX(), networkHeadPos, 80);
	//labelNetworkPauseGameForLaggedClients.init(xoffset+410, networkHeadPos, 80);
	//labelNetworkPauseGameForLaggedClients.setText(lang.getString("NetworkPauseGameForLaggedClients"));

	//checkBoxNetworkPauseGameForLaggedClients.registerGraphicComponent(containerName,"checkBoxNetworkPauseGameForLaggedClients");
	//checkBoxNetworkPauseGameForLaggedClients.init(checkBoxAllowInGameJoinPlayer.getX(), networkHeadPos);
	//checkBoxNetworkPauseGameForLaggedClients.init(xoffset+410, networkHeadPos);
	//checkBoxNetworkPauseGameForLaggedClients.setValue(true);

	//list boxes
	xoffset=30;
	//int rowHeight=27;
    for(int i=0; i<GameConstants::maxPlayers; ++i){

//    	labelPlayers[i].registerGraphicComponent(containerName,"labelPlayers" + intToStr(i));
//		labelPlayers[i].init(xoffset, setupPos-30-i*rowHeight+2);
//		labelPlayers[i].setFont(CoreData::getInstance().getMenuFontBig());
//		labelPlayers[i].setFont3D(CoreData::getInstance().getMenuFontBig3D());

		//labelPlayerStatus[i].registerGraphicComponent(containerName,"labelPlayerStatus" + intToStr(i));
		//labelPlayerStatus[i].init(xoffset+15, setupPos-30-i*rowHeight+2, 60);
		//labelPlayerNames[i].registerGraphicComponent(containerName,"labelPlayerNames" + intToStr(i));
		//labelPlayerNames[i].init(xoffset+30,setupPos-30-i*rowHeight);


        //buttonBlockPlayers[i].registerGraphicComponent(containerName,"buttonBlockPlayers" + intToStr(i));
        //buttonBlockPlayers[i].init(xoffset+355, setupPos-30-i*rowHeight, 70);
        //buttonBlockPlayers[i].init(xoffset+210, setupPos-30-i*rowHeight, 70);
        //buttonBlockPlayers[i].setText(lang.getString("BlockPlayer"));
        //buttonBlockPlayers[i].setFont(CoreData::getInstance().getDisplayFontSmall());
        //buttonBlockPlayers[i].setFont3D(CoreData::getInstance().getDisplayFontSmall3D());

        //listBoxRMultiplier[i].registerGraphicComponent(containerName,"listBoxRMultiplier" + intToStr(i));
        //listBoxRMultiplier[i].init(xoffset+310, setupPos-30-i*rowHeight,70);

        //listBoxFactions[i].registerGraphicComponent(containerName,"listBoxFactions" + intToStr(i));
        //listBoxFactions[i].init(xoffset+390, setupPos-30-i*rowHeight, 250);
        //listBoxFactions[i].setLeftControlled(true);

        //listBoxTeams[i].registerGraphicComponent(containerName,"listBoxTeams" + intToStr(i));
		//listBoxTeams[i].init(xoffset+650, setupPos-30-i*rowHeight, 60);

//		labelNetStatus[i].registerGraphicComponent(containerName,"labelNetStatus" + intToStr(i));
//		labelNetStatus[i].init(xoffset+715, setupPos-30-i*rowHeight, 60);
//		labelNetStatus[i].setFont(CoreData::getInstance().getDisplayFontSmall());
//		labelNetStatus[i].setFont3D(CoreData::getInstance().getDisplayFontSmall3D());
    }

	//buttonClearBlockedPlayers.registerGraphicComponent(containerName,"buttonClearBlockedPlayers");
	//buttonClearBlockedPlayers.init(xoffset+170, setupPos-30-8*rowHeight, 140);

//	labelControl.registerGraphicComponent(containerName,"labelControl");
//	labelControl.init(xoffset+170, setupPos, GraphicListBox::defW, GraphicListBox::defH, true);
//	labelControl.setText(lang.getString("Control"));

    //labelRMultiplier.registerGraphicComponent(containerName,"labelRMultiplier");
	//labelRMultiplier.init(xoffset+310, setupPos, GraphicListBox::defW, GraphicListBox::defH, true);

	//labelFaction.registerGraphicComponent(containerName,"labelFaction");
    //labelFaction.init(xoffset+390, setupPos, GraphicListBox::defW, GraphicListBox::defH, true);
    //labelFaction.setText(lang.getString("Faction"));

    //labelTeam.registerGraphicComponent(containerName,"labelTeam");
    //labelTeam.init(xoffset+650, setupPos, 50, GraphicListBox::defH, true);
    //labelTeam.setText(lang.getString("Team"));

    //labelControl.setFont(CoreData::getInstance().getMenuFontBig());
    //labelControl.setFont3D(CoreData::getInstance().getMenuFontBig3D());
    //labelRMultiplier.setFont(CoreData::getInstance().getMenuFontBig());
    //labelRMultiplier.setFont3D(CoreData::getInstance().getMenuFontBig3D());
	//labelFaction.setFont(CoreData::getInstance().getMenuFontBig());
	//labelFaction.setFont3D(CoreData::getInstance().getMenuFontBig3D());
	//labelTeam.setFont(CoreData::getInstance().getMenuFontBig());
	//labelTeam.setFont3D(CoreData::getInstance().getMenuFontBig3D());

    //xoffset=100;

	//texts
	//buttonClearBlockedPlayers.setText(lang.getString("BlockPlayerClear"));
	//buttonReturn.setText(lang.getString("Return"));
	//buttonPlayNow.setText(lang.getString("PlayNow"));
	//buttonRestoreLastSettings.setText(lang.getString("ReloadLastGameSettings"));

	vector<string> controlItems;
    controlItems.push_back(lang.getString("Closed"));
	controlItems.push_back(lang.getString("CpuEasy"));
	controlItems.push_back(lang.getString("Cpu"));
    controlItems.push_back(lang.getString("CpuUltra"));
    controlItems.push_back(lang.getString("CpuMega"));
	controlItems.push_back(lang.getString("Network"));
	controlItems.push_back(lang.getString("NetworkUnassigned"));
	controlItems.push_back(lang.getString("Human"));

	if(config.getBool("EnableNetworkCpu","false") == true) {
		controlItems.push_back(lang.getString("NetworkCpuEasy"));
		controlItems.push_back(lang.getString("NetworkCpu"));
	    controlItems.push_back(lang.getString("NetworkCpuUltra"));
	    controlItems.push_back(lang.getString("NetworkCpuMega"));
	}

	vector<string> teamItems;
	for(int i = 1; i <= GameConstants::maxPlayers; ++i) {
		teamItems.push_back(intToStr(i));
	}
	for(int i = GameConstants::maxPlayers + 1; i <= GameConstants::maxPlayers + GameConstants::specialFactions; ++i) {
		teamItems.push_back(intToStr(i));
	}

	reloadFactions(false,"");

	if(factionFiles.empty() == true) {
		showGeneralError = true;

		MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
		int selectedTechtreeIndex = cegui_manager.getSelectedItemIndexFromComboBoxControl(cegui_manager.getControl("ComboBoxTechTree"));

		//printf("Line: %d (%d)\n",__LINE__,selectedTechtreeIndex);
		generalErrorToShow = "[#1] There are no factions for the tech tree [" + techTreeFiles.at(selectedTechtreeIndex) + "]";
    }

	for(int i=0; i < GameConstants::maxPlayers; ++i) {
		//labelPlayerStatus[i].setText(" ");
		//labelPlayerStatus[i].setTexture(CoreData::getInstance().getStatusReadyTexture());
		//labelPlayerStatus[i].setH(16);
		//labelPlayerStatus[i].setW(12);

		//labelPlayers[i].setText(lang.getString("Player")+" "+intToStr(i));
		//labelPlayers[i].setText(intToStr(i+1));
		//labelPlayerNames[i].setText("*");
		//labelPlayerNames[i].setMaxEditWidth(16);
		//labelPlayerNames[i].setMaxEditRenderWidth(135);

        //listBoxTeams[i].setItems(teamItems);
		//listBoxTeams[i].setSelectedItemIndex(i);
		//lastSelectedTeamIndex[i] = listBoxTeams[i].getSelectedItemIndex();

		//listBoxRMultiplier[i].setItems(rMultiplier);
		//listBoxRMultiplier[i].setSelectedItem("1.0");
		//labelNetStatus[i].setText("");
    }

	loadMapInfo(Config::getMapPath(getCurrentMapFile()), &mapInfo, true);
	//labelMapInfo.setText(mapInfo.desc);

	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	cegui_manager.setControlText("LabelMapInfo",mapInfo.desc);

	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	//init controllers
	if(serverInitError == false) {
		ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
		if(serverInterface == NULL) {
			throw megaglest_runtime_error("serverInterface == NULL");
		}
		if(this->headlessServerMode == true) {
			updateResourceMultiplier(0);
		}
		else {
			setSlotHuman(0);
			updateResourceMultiplier(0);
		}
		//labelPlayerNames[0].setText("");
		//labelPlayerNames[0].setText(getHumanPlayerName());

		if(openNetworkSlots == true) {
			for(int i= 1; i< mapInfo.players; ++i){
				//listBoxControls[i].setSelectedItemIndex(ctNetwork);
			}
		}
		else{
			//listBoxControls[1].setSelectedItemIndex(ctCpu);
		}
		updateControlers();
		updateNetworkSlots();

		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

		// Ensure we have set the gamesettings at least once
		GameSettings gameSettings;
		loadGameSettings(&gameSettings);

		serverInterface->setGameSettings(&gameSettings,false);
	}
	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	updateAllResourceMultiplier();

	// write hint to console:
	Config &configKeys = Config::getInstance(std::pair<ConfigType,ConfigType>(cfgMainKeys,cfgUserKeys));

	console.addLine(lang.getString("ToSwitchOffMusicPress") + " - \"" + configKeys.getString("ToggleMusic") + "\"");

	chatManager.init(&console, -1,true);

	GraphicComponent::applyAllCustomProperties(containerName);

	static string mutexOwnerId = string(extractFileFromDirectoryPath(__FILE__).c_str()) + string("_") + intToStr(__LINE__);
	publishToMasterserverThread = new SimpleTaskThread(this,0,300,false,(void *)tnt_MASTERSERVER);
	publishToMasterserverThread->setUniqueID(mutexOwnerId);

	static string mutexOwnerId2 = string(extractFileFromDirectoryPath(__FILE__).c_str()) + string("_") + intToStr(__LINE__);
	publishToClientsThread = new SimpleTaskThread(this,0,200,false,(void *)tnt_CLIENTS,false);
	publishToClientsThread->setUniqueID(mutexOwnerId2);

	publishToMasterserverThread->start();
	publishToClientsThread->start();

	if(openNetworkSlots == true) {
		string data_path = getGameReadWritePath(GameConstants::path_data_CacheLookupKey);

		if(fileExists(data_path + DEFAULT_NETWORKGAME_FILENAME) == true)
			loadGameSettings(data_path + DEFAULT_NETWORKGAME_FILENAME);
	}
	else {
		string data_path = getGameReadWritePath(GameConstants::path_data_CacheLookupKey);

		if(fileExists(data_path + DEFAULT_GAME_FILENAME) == true)
			loadGameSettings(data_path + DEFAULT_GAME_FILENAME);
	}
	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	}
	catch(const std::exception &ex) {
	    char szBuf[8096]="";
	    snprintf(szBuf,8096,"In [%s::%s %d]\nError detected:\n%s\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,ex.what());
	    SystemFlags::OutputDebug(SystemFlags::debugError,szBuf);
	    if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s",szBuf);

	    throw megaglest_runtime_error(szBuf);
	}
}

void MenuStateCustomGame::reloadUI() {
	Lang &lang= Lang::getInstance();
    Config &config = Config::getInstance();

    console.resetFonts();
    setupCEGUIWidgetsText(true,false);

	//mainMessageBox.init(lang.getString("Ok"));


//	if(EndsWith(glestVersionString, "-dev") == false){
//		labelLocalGameVersion.setText(glestVersionString);
//	}
//	else {
//		labelLocalGameVersion.setText(glestVersionString + " [" + getCompileDateTime() + ", " + getGITRevisionString() + "]");
//	}

	//vector<string> teamItems, controlItems, results , rMultiplier;

	string ipText = "none";
	std::vector<std::string> ipList = Socket::getLocalIPAddressList();
	if(ipList.empty() == false) {
		ipText = "";
		for(int idx = 0; idx < (int)ipList.size(); idx++) {
			string ip = ipList[idx];
			if(ipText != "") {
				ipText += ", ";
			}
			ipText += ip;
		}
	}
	string serverPort=config.getString("PortServer", intToStr(GameConstants::serverPort).c_str());
	string externalPort=config.getString("PortExternal", serverPort.c_str());

	//labelLocalIP.setText(lang.getString("LanIP") + ipText + "  ( "+serverPort+" / "+externalPort+" )");

	//labelMap.setText(lang.getString("Map")+":");

	//labelMapFilter.setText(lang.getString("MapFilter")+":");

	//labelTileset.setText(lang.getString("Tileset"));

	//labelTechTree.setText(lang.getString("TechTree"));

	//labelAllowNativeLanguageTechtree.setText(lang.getString("AllowNativeLanguageTechtree"));

	//labelFogOfWar.setText(lang.getString("FogOfWar"));

//	std::vector<std::string> listBoxData;
//	listBoxData.push_back(lang.getString("Enabled"));
//	listBoxData.push_back(lang.getString("Explored"));
//	listBoxData.push_back(lang.getString("Disabled"));
//	listBoxFogOfWar.setItems(listBoxData);

	// Allow Observers
	//labelAllowObservers.setText(lang.getString("AllowObservers"));

	// Allow Switch Team Mode
	//labelEnableSwitchTeamMode.setText(lang.getString("EnableSwitchTeamMode"));

	//labelAllowInGameJoinPlayer.setText(lang.getString("AllowInGameJoinPlayer"));

	//labelAllowTeamUnitSharing.setText(lang.getString("AllowTeamUnitSharing"));
	//labelAllowTeamResourceSharing.setText(lang.getString("AllowTeamResourceSharing"));

	//labelAISwitchTeamAcceptPercent.setText(lang.getString("AISwitchTeamAcceptPercent"));

	//listBoxData.clear();

	// Advanced Options
	//labelAdvanced.setText(lang.getString("AdvancedGameOptions"));

	//labelPublishServer.setText(lang.getString("PublishServer"));

//	labelGameName.setFont(CoreData::getInstance().getMenuFontBig());
//	labelGameName.setFont3D(CoreData::getInstance().getMenuFontBig3D());
//	if(this->headlessServerMode == false) {
//		labelGameName.setText(defaultPlayerName+"'s game");
//	}
//	else {
//		labelGameName.setText("headless ("+defaultPlayerName+")");
//	}

	//labelNetworkPauseGameForLaggedClients.setText(lang.getString("NetworkPauseGameForLaggedClients"));

    //for(int i=0; i < GameConstants::maxPlayers; ++i) {
        //buttonBlockPlayers[i].setText(lang.getString("BlockPlayer"));
    //}

	//labelControl.setText(lang.getString("Control"));

    //labelFaction.setText(lang.getString("Faction"));

    //labelTeam.setText(lang.getString("Team"));

    //labelControl.setFont(CoreData::getInstance().getMenuFontBig());
    //labelControl.setFont3D(CoreData::getInstance().getMenuFontBig3D());
    //labelRMultiplier.setFont(CoreData::getInstance().getMenuFontBig());
    //labelRMultiplier.setFont3D(CoreData::getInstance().getMenuFontBig3D());
	//labelFaction.setFont(CoreData::getInstance().getMenuFontBig());
	//labelFaction.setFont3D(CoreData::getInstance().getMenuFontBig3D());
	//labelTeam.setFont(CoreData::getInstance().getMenuFontBig());
	//labelTeam.setFont3D(CoreData::getInstance().getMenuFontBig3D());

	//texts
	//buttonClearBlockedPlayers.setText(lang.getString("BlockPlayerClear"));
	//buttonReturn.setText(lang.getString("Return"));
	//buttonPlayNow.setText(lang.getString("PlayNow"));
	//buttonRestoreLastSettings.setText(lang.getString("ReloadLastGameSettings"));

	vector<string> controlItems;
    controlItems.push_back(lang.getString("Closed"));
	controlItems.push_back(lang.getString("CpuEasy"));
	controlItems.push_back(lang.getString("Cpu"));
    controlItems.push_back(lang.getString("CpuUltra"));
    controlItems.push_back(lang.getString("CpuMega"));
	controlItems.push_back(lang.getString("Network"));
	controlItems.push_back(lang.getString("NetworkUnassigned"));
	controlItems.push_back(lang.getString("Human"));

	if(config.getBool("EnableNetworkCpu","false") == true) {
		controlItems.push_back(lang.getString("NetworkCpuEasy"));
		controlItems.push_back(lang.getString("NetworkCpu"));
	    controlItems.push_back(lang.getString("NetworkCpuUltra"));
	    controlItems.push_back(lang.getString("NetworkCpuMega"));
	}

//	for(int i=0; i < GameConstants::maxPlayers; ++i) {
//		labelPlayers[i].setText(intToStr(i+1));
//
//    }

    //labelFallbackCpuMultiplier.setText(lang.getString("FallbackCpuMultiplier"));

	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

//	vector<string> playerStatuses;
//	playerStatuses.push_back(lang.getString("PlayerStatusSetup"));
//	playerStatuses.push_back(lang.getString("PlayerStatusBeRightBack"));
//	playerStatuses.push_back(lang.getString("PlayerStatusReady"));
//	listBoxPlayerStatus.setItems(playerStatuses);

	//labelScenario.setText(lang.getString("Scenario"));

	reloadFactions(true,(getAllowNetworkScenario() == true ? scenarioFiles.at(getSelectedNetworkScenarioIndex()) : ""));

	// write hint to console:
	Config &configKeys = Config::getInstance(std::pair<ConfigType,ConfigType>(cfgMainKeys,cfgUserKeys));

	console.addLine(lang.getString("ToSwitchOffMusicPress") + " - \"" + configKeys.getString("ToggleMusic") + "\"");

	chatManager.init(&console, -1,true);

	GraphicComponent::reloadFontsForRegisterGraphicComponents(containerName);
}

void MenuStateCustomGame::setupCEGUIWidgets(bool openNetworkSlots) {

	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	cegui_manager.unsubscribeEvents(this->containerName);
	CEGUI::Window *ctl = cegui_manager.setCurrentLayout("CustomGameMenu.layout",containerName);
	cegui_manager.setControlVisible(ctl,true);

	setupCEGUIWidgetsText(false,openNetworkSlots);

/*
	cegui_manager.setControlEventCallback(containerName, "TabControl",
					cegui_manager.getEventTabControlSelectionChanged(), this);


	cegui_manager.setControlEventCallback(containerName,
			"TabControl/__auto_TabPane__/Misc/ComboBoxLanguage",
					cegui_manager.getEventComboboxClicked(), this);

	cegui_manager.setControlEventCallback(containerName,
			"TabControl/__auto_TabPane__/Misc/ComboBoxLanguage",
					cegui_manager.getEventComboboxChangeAccepted(), this);

	cegui_manager.setControlEventCallback(containerName,
			"TabControl/__auto_TabPane__/Misc/CheckboxDisableLuaSandbox",
			cegui_manager.getEventCheckboxClicked(), this);

	cegui_manager.setControlEventCallback(containerName,
			"TabControl/__auto_TabPane__/Misc/CheckboxAdvancedTranslation",
			cegui_manager.getEventCheckboxClicked(), this);

	cegui_manager.setControlEventCallback(containerName,
			"TabControl/__auto_TabPane__/Misc/GroupBoxAdvancedTranslation/__auto_contentpane__/ButtonDownloadFromTransifex",
			cegui_manager.getEventButtonClicked(), this);

	cegui_manager.setControlEventCallback(containerName,
			"TabControl/__auto_TabPane__/Misc/GroupBoxAdvancedTranslation/__auto_contentpane__/ButtonDeleteDownloadedTransifexFiles",
			cegui_manager.getEventButtonClicked(), this);

	cegui_manager.setControlEventCallback(containerName,
			"TabControl/__auto_TabPane__/Misc/ButtonSave",
			cegui_manager.getEventButtonClicked(), this);

	cegui_manager.setControlEventCallback(containerName,
			"TabControl/__auto_TabPane__/Misc/ButtonReturn",
			cegui_manager.getEventButtonClicked(), this);

	cegui_manager.subscribeMessageBoxEventClicks(containerName, this);
	cegui_manager.subscribeMessageBoxEventClicks(containerName, this, "TabControl/__auto_TabPane__/Misc/LuaMsgBox");
*/

	cegui_manager.setControlEventCallback(containerName, "ComboMapFilter",
					cegui_manager.getEventComboboxChangeAccepted(), this);

	cegui_manager.setControlEventCallback(containerName, "ComboMap",
					cegui_manager.getEventComboboxChangeAccepted(), this);

	cegui_manager.setControlEventCallback(containerName, "ComboBoxTechTree",
					cegui_manager.getEventComboboxChangeAccepted(), this);

	cegui_manager.setControlEventCallback(containerName, "ComboBoxTileset",
					cegui_manager.getEventComboboxChangeAccepted(), this);

	cegui_manager.setControlEventCallback(containerName, "ComboBoxFogOfWar",
					cegui_manager.getEventComboboxChangeAccepted(), this);

	cegui_manager.setControlEventCallback(containerName, "ComboHumanPlayerStatus",
					cegui_manager.getEventComboboxChangeAccepted(), this);

	for(int index = 0; index < GameConstants::maxPlayers; ++index) {

		cegui_manager.setControlEventCallback(containerName,
				"ComboBoxPlayer" + intToStr(index+1) + "Control",
						cegui_manager.getEventComboboxChangeAccepted(), this);

		cegui_manager.setControlEventCallback(containerName,
				"ComboBoxPlayer" + intToStr(index+1) + "Faction",
						cegui_manager.getEventComboboxChangeAccepted(), this);

		cegui_manager.setControlEventCallback(containerName,
				"ComboBoxPlayer" + intToStr(index+1) + "Team",
						cegui_manager.getEventComboboxChangeAccepted(), this);


		cegui_manager.setControlEventCallback(containerName,"ButtonPlayer" + intToStr(index+1) + "Block",
					cegui_manager.getEventButtonClicked(), this);
	}

	cegui_manager.setControlEventCallback(containerName,"ButtonClearBlockedPlayers",
						cegui_manager.getEventButtonClicked(), this);

	cegui_manager.setControlEventCallback(containerName,
			"ComboNetworkScenarios",
					cegui_manager.getEventComboboxChangeAccepted(), this);

	cegui_manager.setControlEventCallback(containerName,
			"ComboAIAcceptPercent",
					cegui_manager.getEventComboboxChangeAccepted(), this);



	cegui_manager.setControlEventCallback(containerName, "CheckboxAllowSwitchTeams",
					cegui_manager.getEventCheckboxClicked(), this);

	cegui_manager.setControlEventCallback(containerName, "CheckboxAllowObservers",
					cegui_manager.getEventCheckboxClicked(), this);


	cegui_manager.setControlEventCallback(containerName, "CheckboxAllowInProgressJoinGame",
					cegui_manager.getEventCheckboxClicked(), this);

	cegui_manager.setControlEventCallback(containerName, "CheckboxAllowSharedTeamUnits",
					cegui_manager.getEventCheckboxClicked(), this);

	cegui_manager.setControlEventCallback(containerName, "CheckboxAllowSharedTeamResources",
					cegui_manager.getEventCheckboxClicked(), this);

	cegui_manager.setControlEventCallback(containerName, "CheckboxTechTreeTranslated",
					cegui_manager.getEventCheckboxClicked(), this);

	cegui_manager.setControlEventCallback(containerName, "CheckboxPauseLaggingClients",
					cegui_manager.getEventCheckboxClicked(), this);

	cegui_manager.setControlEventCallback(containerName, "CheckboxPublishGame",
					cegui_manager.getEventCheckboxClicked(), this);

	cegui_manager.setControlEventCallback(containerName, "CheckboxShowNetworkScenarios",
					cegui_manager.getEventCheckboxClicked(), this);


	cegui_manager.setControlEventCallback(containerName,"ButtonReturn",
			cegui_manager.getEventButtonClicked(), this);

	cegui_manager.setControlEventCallback(containerName,"ButtonReloadLastSettings",
			cegui_manager.getEventButtonClicked(), this);

	cegui_manager.setControlEventCallback(containerName,"ButtonPlay",
			cegui_manager.getEventButtonClicked(), this);

	cegui_manager.subscribeMessageBoxEventClicks(containerName, this);
}

void MenuStateCustomGame::setupCEGUIWidgetsText(bool isReload, bool openNetworkSlots) {

	Lang &lang		= Lang::getInstance();
	Config &config	= Config::getInstance();

	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	cegui_manager.setCurrentLayout("CustomGameMenu.layout",containerName);

	string ipText = "none";
	std::vector<std::string> ipList = Socket::getLocalIPAddressList();
	if(ipList.empty() == false) {
		ipText = "";
		for(int idx = 0; idx < (int)ipList.size(); idx++) {
			string ip = ipList[idx];
			if(ipText != "") {
				ipText += ", ";
			}
			ipText += ip;
		}
	}
	string serverPort 	= config.getString("PortServer", intToStr(GameConstants::serverPort).c_str());
	string externalPort = config.getString("PortExternal", serverPort.c_str());
	cegui_manager.setControlText("LabelLocalGameIPTitle",
			lang.getString("LanIP","",false,true) +
			ipText +
			"  ( " + serverPort + " / " + externalPort + " )");
	//ServerSocket::setExternalPort(strToInt(externalPort));

	if(EndsWith(glestVersionString, "-dev") == false){
		cegui_manager.setControlText("LabelLocalGameVersion",glestVersionString);

	}
	else {
		cegui_manager.setControlText("LabelLocalGameVersion",glestVersionString + " [colour='FF00FF00'] \\[" + getCompileDateTime() + ", " + getGITRevisionString() + "]");
	}

	cegui_manager.setControlText("LabelPublishGame",lang.getString("PublishServer","",false,true));
	if(isReload == false) {
		cegui_manager.setCheckboxControlChecked(cegui_manager.getControl("CheckboxPublishGame"),
				(this->headlessServerMode == true ||
						(openNetworkSlots == true && parentMenuState != pLanGame)) &&
							GlobalStaticFlags::isFlagSet(gsft_lan_mode) == false);

		if(this->headlessServerMode == false) {
			cegui_manager.setControlText("EditboxPublishGameName",defaultPlayerName + "'s game");
		}
		else {
			cegui_manager.setControlText("EditboxPublishGameName","headless (" + defaultPlayerName +")");
		}
	}

	cegui_manager.setControlText("LabelTechTree",lang.getString("TechTree","",false,true));

    //tech Tree listBox
    int initialTechSelection = setupTechList("", true);
    if(initialTechSelection >= 0) {
    	cegui_manager.setSelectedItemInComboBoxControl(
			cegui_manager.getControl("ComboBoxTechTree"), initialTechSelection);
    }

    cegui_manager.setControlText("LabelTechTreeTranslated",lang.getString("AllowNativeLanguageTechtree","",false,true));
    if(isReload == false) {
    	cegui_manager.setCheckboxControlChecked(cegui_manager.getControl("CheckboxTechTreeTranslated"),false);
    }

    cegui_manager.setControlText("LabelShowAdvancedOptions",lang.getString("AdvancedGameOptions","",false,true));
    if(isReload == false) {
    	cegui_manager.setCheckboxControlChecked(cegui_manager.getControl("CheckboxShowAdvancedOptions"),false);
    }

    cegui_manager.setControlText("LabelMapFilter",lang.getString("MapFilter","",false,true));
    vector<string> values;
    values.push_back("-");
    for(int index = 1; index < GameConstants::maxPlayers+1; ++index) {
    	values.push_back(intToStr(index));
    }
	cegui_manager.addItemsToComboBoxControl(
			cegui_manager.getControl("ComboMapFilter"), values);
	//cegui_manager.setSelectedItemInComboBoxControl(
	//	cegui_manager.getControl("ComboMapFilter"), 0);

	cegui_manager.setControlText("LabelMap",lang.getString("Map","",false,true));

	// put them all in a set, to weed out duplicates (gbm & mgm with same name)
	// will also ensure they are alphabetically listed (rather than how the OS provides them)
	string initialMapSelection = setupMapList("");

	cegui_manager.addItemsToComboBoxControl(
			cegui_manager.getControl("ComboMap"), formattedPlayerSortedMaps[0]);
//	cegui_manager.setSelectedItemInComboBoxControl(
//		cegui_manager.getControl("ComboMap"), initialMapSelection);

	// Default to 4 player maps (index 3)
	switchToMapGroup(0);
	cegui_manager.setSelectedItemInComboBoxControl(
		cegui_manager.getControl("ComboMap"), initialMapSelection);

	loadMapInfo(Config::getMapPath(getCurrentMapFile()), &mapInfo, true);
	cegui_manager.setControlText("LabelMapInfo",mapInfo.desc);
	//printf("Selected Map: %s",mapInfo.desc.c_str());

	cegui_manager.setControlText("LabelTileset",lang.getString("Tileset","",false,true));

	setupTilesetList("");
	Chrono seed(true);
	srand((unsigned int)seed.getCurTicks());
	int selectedTileset = rand() % tilesetFiles.size();
	cegui_manager.setSelectedItemInComboBoxControl(
		cegui_manager.getControl("ComboBoxTileset"), selectedTileset);


	cegui_manager.setControlText("LabelFogOfWar",lang.getString("FogOfWar","",false,true));

    values.clear();
    values.push_back(lang.getString("Enabled"));
    values.push_back(lang.getString("Explored"));
    values.push_back(lang.getString("Disabled"));

	cegui_manager.addItemsToComboBoxControl(
			cegui_manager.getControl("ComboBoxFogOfWar"), values);
	cegui_manager.setSelectedItemInComboBoxControl(
		cegui_manager.getControl("ComboBoxFogOfWar"), 0);

    values.clear();
    values.push_back(lang.getString("PlayerStatusSetup"));
    values.push_back(lang.getString("PlayerStatusBeRightBack"));
    values.push_back(lang.getString("PlayerStatusReady"));

	cegui_manager.addItemsToComboBoxControl(
			cegui_manager.getControl("ComboHumanPlayerStatus"), values);
	cegui_manager.setSelectedItemInComboBoxControl(
		cegui_manager.getControl("ComboHumanPlayerStatus"), 2);

	cegui_manager.setControlText("LabelAIAcceptPercent",lang.getString("AISwitchTeamAcceptPercent","",false,true));

	values.clear();
	for(int index = 0; index <= 100; index += 10) {
		values.push_back(intToStr(index));
	}
	cegui_manager.addItemsToComboBoxControl(
			cegui_manager.getControl("ComboAIAcceptPercent"), values);
	cegui_manager.setSelectedItemInComboBoxControl(
		cegui_manager.getControl("ComboAIAcceptPercent"), 4);

	cegui_manager.setControlText("LabelAIReplaceMultiplier",lang.getString("FallbackCpuMultiplier","",false,true));
	cegui_manager.setSpinnerControlValues(cegui_manager.getControl("SpinnerAIReplaceMultiplier"),0,5,1.0,0.1);

	cegui_manager.setControlText("LabelAllowSwitchTeams",lang.getString("EnableSwitchTeamMode","",false,true));
	cegui_manager.setCheckboxControlChecked(cegui_manager.getControl("CheckboxAllowSwitchTeams"),false);

	cegui_manager.setControlText("LabelAllowObservers",lang.getString("AllowObservers","",false,true));
	cegui_manager.setCheckboxControlChecked(cegui_manager.getControl("CheckboxAllowObservers"),false);

	cegui_manager.setControlText("LabelAllowSharedTeamUnits",lang.getString("AllowTeamUnitSharing","",false,true));
	cegui_manager.setCheckboxControlChecked(cegui_manager.getControl("CheckboxAllowSharedTeamUnits"),false);

	cegui_manager.setControlText("LabelAllowSharedTeamResources",lang.getString("AllowTeamResourceSharing","",false,true));
	cegui_manager.setCheckboxControlChecked(cegui_manager.getControl("CheckboxAllowSharedTeamResources"),false);

	cegui_manager.setControlText("LabelShowNetworkScenarios",lang.getString("Scenario","",false,true));
	cegui_manager.setCheckboxControlChecked(cegui_manager.getControl("CheckboxShowNetworkScenarios"),false);

	cegui_manager.setControlText("LabelPauseLaggingClients",lang.getString("NetworkPauseGameForLaggedClients","",false,true));
	cegui_manager.setCheckboxControlChecked(cegui_manager.getControl("CheckboxPauseLaggingClients"),true);

	bool allowInProgressJoin = Config::getInstance().getBool("EnableJoinInProgressGame","false");

	cegui_manager.setControlText("LabelAllowInProgressJoinGame",lang.getString("AllowInGameJoinPlayer","",false,true));
	cegui_manager.setCheckboxControlChecked(cegui_manager.getControl("CheckboxAllowInProgressJoinGame"),false);
	cegui_manager.setControlVisible("LabelAllowInProgressJoinGame", allowInProgressJoin);
	cegui_manager.setControlVisible("CheckboxAllowInProgressJoinGame", allowInProgressJoin);


	vector<string> controlItems;
    controlItems.push_back(lang.getString("Closed"));
	controlItems.push_back(lang.getString("CpuEasy"));
	controlItems.push_back(lang.getString("Cpu"));
    controlItems.push_back(lang.getString("CpuUltra"));
    controlItems.push_back(lang.getString("CpuMega"));
	controlItems.push_back(lang.getString("Network"));
	controlItems.push_back(lang.getString("NetworkUnassigned"));
	controlItems.push_back(lang.getString("Human"));

	if(config.getBool("EnableNetworkCpu","false") == true) {
		controlItems.push_back(lang.getString("NetworkCpuEasy"));
		controlItems.push_back(lang.getString("NetworkCpu"));
	    controlItems.push_back(lang.getString("NetworkCpuUltra"));
	    controlItems.push_back(lang.getString("NetworkCpuMega"));
	}

	vector<string> teamItems;
	for(int index = 1; index <= GameConstants::maxPlayers; ++index) {
		teamItems.push_back(intToStr(index));
	}
	for(int index = GameConstants::maxPlayers + 1; index <= GameConstants::maxPlayers + GameConstants::specialFactions; ++index) {
		teamItems.push_back(intToStr(index));
	}

	std::map<int,Texture2D *> &crcPlayerTextureCache =
			CacheManager::getCachedItem< std::map<int,Texture2D *> >(
					GameConstants::playerTextureCacheLookupKey);

	for(int index = 0; index < GameConstants::maxPlayers; ++index) {

		cegui_manager.setControlText("LabelPlayer" + intToStr(index+1) + "Number",intToStr(index+1));
		Vec3f playerColor = crcPlayerTextureCache[index]->getPixmap()->getPixel3f(0, 0);
		cegui_manager.setControlTextColor("LabelPlayer" + intToStr(index+1) + "Number",
				playerColor.x, playerColor.y, playerColor.z);

		//cegui_manager.setImageForControl("ImagePlayer" + intToStr(index+1) + "Status_Texture",CoreData::getInstance().getStatusReadyTexture(), "ImagePlayer" + intToStr(index+1) + "Status", true);
		setPlayerStatusImage(index, CoreData::getInstance().getStatusReadyTexture());

		string cegui_playercolor = cegui_manager.getTextColorFromRGBA(playerColor.x, playerColor.y, playerColor.z);
		setPlayerNameText(index,"*");


		cegui_manager.addItemsToComboBoxControl(
				cegui_manager.getControl("ComboBoxPlayer" + intToStr(index+1) + "Control"), controlItems);
		//cegui_manager.setSelectedItemInComboBoxControl(
		//	cegui_manager.getControl("ComboBoxPlayer1Control"), 4);

		//ComboBoxPlayer1Multiplier
		cegui_manager.setSpinnerControlValues(
				cegui_manager.getControl(
						"SpinnerPlayer" + intToStr(index+1) + "Multiplier"),
						0,5,1.0,0.1);
//		printf("FILE: [%s: %d] value: %f\n",__FILE__,__LINE__,
//				cegui_manager.getSpinnerControlValue(cegui_manager.getControl(
//						"SpinnerPlayer" + intToStr(index+1) + "Multiplier")));

		//ComboBoxPlayer1Faction

		cegui_manager.addItemsToComboBoxControl(
				cegui_manager.getControl("ComboBoxPlayer" + intToStr(index+1) + "Team"), teamItems);
		cegui_manager.setSelectedItemInComboBoxControl(
			cegui_manager.getControl("ComboBoxPlayer" + intToStr(index+1) + "Team"), index);

		lastSelectedTeamIndex[index] = index;

		//cegui_manager.setControlText("LabelPlayer" + intToStr(index+1) + "Version","*");
		cegui_manager.setControlText("LabelPlayer" + intToStr(index+1) + "Version","");


		//buttonBlockPlayers
		cegui_manager.setControlText("ButtonPlayer" + intToStr(index+1) + "Block",lang.getString("BlockPlayer","",false,true));
		cegui_manager.setControlVisible("ButtonPlayer" + intToStr(index+1) + "Block",false);
	}

	cegui_manager.setControlText("ButtonClearBlockedPlayers",lang.getString("BlockPlayerClear","",false,true));
	cegui_manager.setControlVisible("ButtonClearBlockedPlayers",false);

	//init controllers
	if(serverInitError == false) {
		ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
		if(serverInterface == NULL) {
			throw megaglest_runtime_error("serverInterface == NULL");
		}
		if(this->headlessServerMode == true) {
			//listBoxControls[0].setSelectedItemIndex(ctNetwork);
			cegui_manager.setSelectedItemInComboBoxControl(
					cegui_manager.getControl("ComboBoxPlayer1Control"), ctNetwork);

			//updateResourceMultiplier(0);
		}
		else {
			setSlotHuman(0);
			//updateResourceMultiplier(0);
		}
		//labelPlayerNames[0].setText("");
		//labelPlayerNames[0].setText(getHumanPlayerName());
		setPlayerNameText(0,"");
		setPlayerNameText(0,getHumanPlayerName());

		if(openNetworkSlots == true) {
			for(int index = 1; index < mapInfo.players; ++index){
				//listBoxControls[i].setSelectedItemIndex(ctNetwork);
				cegui_manager.setSelectedItemInComboBoxControl(
						cegui_manager.getControl("ComboBoxPlayer" + intToStr(index+1) + "Control"), ctNetwork);
			}
		}
		else{
			//listBoxControls[1].setSelectedItemIndex(ctCpu);
			cegui_manager.setSelectedItemInComboBoxControl(
					cegui_manager.getControl("ComboBoxPlayer2Control"), ctCpu);
		}
		//updateControlers();
		//updateNetworkSlots();

		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

		// Ensure we have set the gamesettings at least once
		//GameSettings gameSettings;
		//loadGameSettings(&gameSettings);

		//serverInterface->setGameSettings(&gameSettings,false);
	}



    //scenario listbox
    vector<string> resultsScenarios;
	findDirs(dirList, resultsScenarios);
	// Filter out only scenarios with no network slots
	for(int i= 0; i < (int)resultsScenarios.size(); ++i) {
		string scenario = resultsScenarios[i];
		string file = Scenario::getScenarioPath(dirList, scenario);

		try {
			if(file != "") {
				bool isTutorial = Scenario::isGameTutorial(file);
				Scenario::loadScenarioInfo(file, &scenarioInfo, isTutorial);

				bool isNetworkScenario = false;
				for(unsigned int j = 0; isNetworkScenario == false && j < (unsigned int)GameConstants::maxPlayers; ++j) {
					if(scenarioInfo.factionControls[j] == ctNetwork) {
						isNetworkScenario = true;
					}
				}
				if(isNetworkScenario == true) {
					scenarioFiles.push_back(scenario);
				}
			}
		}
		catch(const std::exception &ex) {
		    char szBuf[8096]="";
		    snprintf(szBuf,8096,"In [%s::%s %d]\nError loading scenario [%s]:\n%s\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,scenario.c_str(),ex.what());
		    SystemFlags::OutputDebug(SystemFlags::debugError,szBuf);
		    if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s",szBuf);

		    showGeneralError=true;
			generalErrorToShow = szBuf;
		    //throw megaglest_runtime_error(szBuf);
		}
	}
	resultsScenarios.clear();
	for(int i = 0; i < (int)scenarioFiles.size(); ++i) {
		resultsScenarios.push_back(formatString(scenarioFiles.at(i)));
	}

	cegui_manager.addItemsToComboBoxControl(
			cegui_manager.getControl("ComboNetworkScenarios"), resultsScenarios);
	//cegui_manager.setSelectedItemInComboBoxControl(
	//	cegui_manager.getControl("ComboNetworkScenarios"), index);

	setNetworkScenarioVisible(getAllowNetworkScenario());

    if(resultsScenarios.empty() == true) {
    	setAllowNetworkScenario(false);
    }

	cegui_manager.setControlText("EditboxChatText","");
	cegui_manager.setControlText("MultiLineEditboxChatHistory","");

	cegui_manager.setControlText("ButtonReturn",lang.getString("Return","",false,true));
	cegui_manager.setControlText("ButtonReloadLastSettings",lang.getString("ReloadLastGameSettings","",false,true));
	cegui_manager.setControlText("ButtonPlay",lang.getString("PlayNow","",false,true));

	cegui_manager.hideMessageBox();

/*
	CEGUI::Window *ctlAudio 	= cegui_manager.loadLayoutFromFile("OptionsMenuAudio.layout");
	CEGUI::Window *ctlKeyboard 	= cegui_manager.loadLayoutFromFile("OptionsMenuKeyboard.layout");
	CEGUI::Window *ctlMisc 		= cegui_manager.loadLayoutFromFile("OptionsMenuMisc.layout");
	CEGUI::Window *ctlNetwork 	= cegui_manager.loadLayoutFromFile("OptionsMenuNetwork.layout");
	CEGUI::Window *ctlVideo 	= cegui_manager.loadLayoutFromFile("OptionsMenuVideo.layout");

	cegui_manager.setControlText(ctlAudio,lang.getString("Audio","",false,true));
	cegui_manager.setControlText(ctlKeyboard,lang.getString("Keyboardsetup","",false,true));
	cegui_manager.setControlText(ctlMisc,lang.getString("Misc","",false,true));
	cegui_manager.setControlText(ctlNetwork,lang.getString("Network","",false,true));
	cegui_manager.setControlText(ctlVideo,lang.getString("Video","",false,true));

	if(cegui_manager.isChildControl(cegui_manager.getControl("TabControl"),"__auto_TabPane__/Audio") == false) {
		cegui_manager.addTabPageToTabControl("TabControl", ctlAudio,"",18);
	}
	if(cegui_manager.isChildControl(cegui_manager.getControl("TabControl"),"__auto_TabPane__/Keyboard") == false) {
		cegui_manager.addTabPageToTabControl("TabControl", ctlKeyboard,"",18);
	}
	if(cegui_manager.isChildControl(cegui_manager.getControl("TabControl"),"__auto_TabPane__/Misc") == false) {
		cegui_manager.addTabPageToTabControl("TabControl", ctlMisc,"",18);
	}
	if(cegui_manager.isChildControl(cegui_manager.getControl("TabControl"),"__auto_TabPane__/Network") == false) {
		cegui_manager.addTabPageToTabControl("TabControl", ctlNetwork,"",18);
	}
	if(cegui_manager.isChildControl(cegui_manager.getControl("TabControl"),"__auto_TabPane__/Video") == false) {
		cegui_manager.addTabPageToTabControl("TabControl", ctlVideo,"",18);
	}

	cegui_manager.setSelectedTabPage("TabControl", "Misc");

	//cegui_manager.dumpWindowNames("Test #1");
	if(cegui_manager.isChildControl(cegui_manager.getControl("TabControl/__auto_TabPane__/Misc"),"LuaMsgBox") == false) {
		cegui_manager.cloneMessageBoxControl("LuaMsgBox", ctlMisc);
	}
	cegui_manager.setControlText(cegui_manager.getChildControl(ctlMisc,"LabelLanguage"),lang.getString("Language","",false,true));

	//languageList = lang.getDiscoveredLanguageList(true);
	pair<string,string> defaultLang = lang.getNavtiveNameFromLanguageName(config.getString("Lang"));
	if(defaultLang.first == "" && defaultLang.second == "") {
		defaultLang = lang.getNavtiveNameFromLanguageName(lang.getDefaultLanguage());
	}
	string defaultLanguageText = defaultLang.second + "-" + defaultLang.first;
	string defaultLanguageTextFormatted = defaultLanguageText;

	//int langCount = 0;
	map<string,int> langResultsMap;
	vector<string> langResults;
//	for(map<string,string>::iterator iterMap = languageList.begin();
//			iterMap != languageList.end(); ++iterMap) {
//
//		string language = iterMap->first + "-" + iterMap->second;
//		string languageFont = "";
//		if(lang.hasString("MEGAGLEST_FONT",iterMap->first) == true) {
//
//			bool langIsDefault = false;
//			if(defaultLanguageText == language) {
//				langIsDefault = true;
//			}
//			string fontFile = lang.getString("MEGAGLEST_FONT",iterMap->first);
//			if(	lang.hasString("MEGAGLEST_FONT_FAMILY")) {
//				string fontFamily = lang.getString("MEGAGLEST_FONT_FAMILY",iterMap->first);
//				fontFile = findFont(fontFile.c_str(),fontFamily.c_str());
//			}
//
//			cegui_manager.addFont("MEGAGLEST_FONT_" + iterMap->first, fontFile, 10.0f);
//			language = iterMap->first + "-[font='" + "MEGAGLEST_FONT_" + iterMap->first + "-10.00']" + iterMap->second;
//
//			if(langIsDefault == true) {
//				defaultLanguageTextFormatted = language;
//			}
//		}
//		langResults.push_back(language);
//		langResultsMap[language] = langCount;
//		langCount++;
//	}

	cegui_manager.addItemsToComboBoxControl(
			cegui_manager.getChildControl(ctlMisc,"ComboBoxLanguage"), langResultsMap,false);

	cegui_manager.setSelectedItemInComboBoxControl(
			cegui_manager.getChildControl(ctlMisc,"ComboBoxLanguage"), defaultLanguageTextFormatted,false);
	cegui_manager.setControlText(cegui_manager.getChildControl(ctlMisc,"ComboBoxLanguage"),defaultLanguageText);

	cegui_manager.setControlText(cegui_manager.getChildControl(ctlMisc,"LabelPlayerName"),lang.getString("Playername","",false,true));
	cegui_manager.setControlText(cegui_manager.getChildControl(ctlMisc,"EditboxPlayerName"),config.getString("NetPlayerName",Socket::getHostName().c_str()));

	cegui_manager.setControlText(cegui_manager.getChildControl(ctlMisc,"LabelFontAdjustment"),lang.getString("FontSizeAdjustment","",false,true));
	cegui_manager.setSpinnerControlValues(cegui_manager.getChildControl(ctlMisc,"SpinnerFontAdjustment"),-5,5,config.getInt("FontSizeAdjustment"));

	cegui_manager.setControlText(cegui_manager.getChildControl(ctlMisc,"LabelScreenshotFormat"),lang.getString("ScreenShotFileType","",false,true));
	vector<string> langScreenshotFormats;
	langScreenshotFormats.push_back("bmp");
	langScreenshotFormats.push_back("jpg");
	langScreenshotFormats.push_back("png");
	langScreenshotFormats.push_back("tga");

	cegui_manager.addItemsToComboBoxControl(
			cegui_manager.getChildControl(ctlMisc,"ComboBoxScreenshotFormat"), langScreenshotFormats);
	cegui_manager.setSelectedItemInComboBoxControl(
			cegui_manager.getChildControl(ctlMisc,"ComboBoxScreenshotFormat"), config.getString("ScreenShotFileType","jpg"));

	cegui_manager.setControlText(cegui_manager.getChildControl(ctlMisc,"LabelShowScreenshotSaved"),lang.getString("ScreenShotConsoleText","",false,true));
	cegui_manager.setCheckboxControlChecked(cegui_manager.getChildControl(ctlMisc,"CheckboxShowScreenshotSaved"),!config.getBool("DisableScreenshotConsoleText","false"));

	cegui_manager.setControlText(cegui_manager.getChildControl(ctlMisc,"LabelMouseMovesCamera"),lang.getString("MouseScrollsWorld","",false,true));
	cegui_manager.setCheckboxControlChecked(cegui_manager.getChildControl(ctlMisc,"CheckboxMouseMovesCamera"),config.getBool("MouseMoveScrollsWorld","true"));

	cegui_manager.setControlText(cegui_manager.getChildControl(ctlMisc,"LabelCameraMoveSpeed"),lang.getString("CameraMoveSpeed","",false,true));
	cegui_manager.setSliderControlValues(cegui_manager.getChildControl(ctlMisc,"SliderCameraMoveSpeed"),15,50,config.getFloat("CameraMoveSpeed","15"),1);

	cegui_manager.setControlText(cegui_manager.getChildControl(ctlMisc,"LabelVisibleHUD"),lang.getString("VisibleHUD","",false,true));
	cegui_manager.setCheckboxControlChecked(cegui_manager.getChildControl(ctlMisc,"CheckboxVisibleHUD"),config.getBool("VisibleHud","true"));

	cegui_manager.setControlText(cegui_manager.getChildControl(ctlMisc,"LabelChatRemainsActive"),lang.getString("ChatStaysActive","",false,true));
	cegui_manager.setCheckboxControlChecked(cegui_manager.getChildControl(ctlMisc,"CheckboxChatRemainsActive"),config.getBool("ChatStaysActive","false"));

	cegui_manager.setControlText(cegui_manager.getChildControl(ctlMisc,"LabelDisplayRealAndGameTime"),lang.getString("TimeDisplay","",false,true));
	cegui_manager.setCheckboxControlChecked(cegui_manager.getChildControl(ctlMisc,"CheckboxDisplayRealAndGameTime"),config.getBool("TimeDisplay","true"));

	cegui_manager.setControlText(cegui_manager.getChildControl(ctlMisc,"LabelDisableLuaSandbox"),lang.getString("LuaDisableSecuritySandbox","",false,true));
	cegui_manager.setCheckboxControlChecked(cegui_manager.getChildControl(ctlMisc,"CheckboxDisableLuaSandbox"),config.getBool("DisableLuaSandbox","false"));

	cegui_manager.setControlText(cegui_manager.getChildControl(ctlMisc,"LabelAdvancedTranslation"),lang.getString("CustomTranslation","",false,true));

	cegui_manager.setCheckboxControlChecked(cegui_manager.getChildControl(ctlMisc,"CheckboxAdvancedTranslation"),false);

	cegui_manager.setControlText("TabControl/__auto_TabPane__/Misc/GroupBoxAdvancedTranslation/__auto_contentpane__/LabelTransifexUsername",lang.getString("TransifexUserName","",false,true));
	cegui_manager.setControlText("TabControl/__auto_TabPane__/Misc/GroupBoxAdvancedTranslation/__auto_contentpane__/EditboxTransifexUsername",config.getString("TranslationGetURLUser","<none>"));

	cegui_manager.setControlText("TabControl/__auto_TabPane__/Misc/GroupBoxAdvancedTranslation/__auto_contentpane__/LabelTransifexPassword",lang.getString("TransifexPwd","",false,true));
	cegui_manager.setControlText("TabControl/__auto_TabPane__/Misc/GroupBoxAdvancedTranslation/__auto_contentpane__/EditboxTransifexPassword",config.getString("TranslationGetURLPassword",""));

	cegui_manager.setControlText("TabControl/__auto_TabPane__/Misc/GroupBoxAdvancedTranslation/__auto_contentpane__/LabelTransifexLanguageCode",lang.getString("TransifexI18N","",false,true));
	cegui_manager.setControlText("TabControl/__auto_TabPane__/Misc/GroupBoxAdvancedTranslation/__auto_contentpane__/EditboxTransifexLanguageCode",config.getString("TranslationGetURLLanguage","en"));

	cegui_manager.setControlText("TabControl/__auto_TabPane__/Misc/GroupBoxAdvancedTranslation/__auto_contentpane__/ButtonDownloadFromTransifex",lang.getString("TransifexGetLanguageFiles","",false,true));
	cegui_manager.setControlText("TabControl/__auto_TabPane__/Misc/GroupBoxAdvancedTranslation/__auto_contentpane__/ButtonDeleteDownloadedTransifexFiles",lang.getString("TransifexDeleteLanguageFiles","",false,true));

	cegui_manager.setControlText("TabControl/__auto_TabPane__/Misc/ButtonSave",lang.getString("Save","",false,true));
	cegui_manager.setControlText("TabControl/__auto_TabPane__/Misc/ButtonReturn",lang.getString("Return","",false,true));

	cegui_manager.hideMessageBox();
	cegui_manager.hideMessageBox("TabControl/__auto_TabPane__/Misc/LuaMsgBox");

	//throw runtime_error("test!");

*/
}

void MenuStateCustomGame::callDelayedCallbacks() {
	if(hasDelayedCallbacks() == true) {
		while(hasDelayedCallbacks() == true) {
			DelayCallbackFunction pCB = delayedCallbackList[0];
			delayedCallbackList.erase(delayedCallbackList.begin());
			bool hasMoreCallbacks = hasDelayedCallbacks();
			(this->*pCB)();
			if(hasMoreCallbacks == false) {
				return;
			}
		}
	}
}

void MenuStateCustomGame::delayedCallbackFunctionReturn() {
	CoreData &coreData				= CoreData::getInstance();
	SoundRenderer &soundRenderer	= SoundRenderer::getInstance();

	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	cegui_manager.unsubscribeEvents(this->containerName);

	soundRenderer.playFx(coreData.getClickSoundA());

	MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
	MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
	needToBroadcastServerSettings = false;
	needToRepublishToMasterserver = false;
	lastNetworkPing               = time(NULL);
	safeMutex.ReleaseLock();
	safeMutexCLI.ReleaseLock();

	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	returnToParentMenu();
}

void MenuStateCustomGame::delayedCallbackFunctionPlayNow() {

	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	cegui_manager.unsubscribeEvents(this->containerName);

	PlayNow(true);
}

void MenuStateCustomGame::delayedCallbackFunctionReloadLastSettings() {

	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	cegui_manager.unsubscribeEvents(this->containerName);

	CoreData &coreData				= CoreData::getInstance();
	SoundRenderer &soundRenderer	= SoundRenderer::getInstance();

	soundRenderer.playFx(coreData.getClickSoundB());
	RestoreLastGameSettings();
}

bool MenuStateCustomGame::EventCallback(CEGUI::Window *ctl, std::string name) {

	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();

	//printf("In EventCallback: event [%s]\n",name.c_str());

	if(name == cegui_manager.getEventButtonClicked()) {

		if(cegui_manager.isControlMessageBoxOk(ctl) == true) {

			CoreData &coreData				= CoreData::getInstance();
			SoundRenderer &soundRenderer	= SoundRenderer::getInstance();

			soundRenderer.playFx(coreData.getClickSoundA());
			if(mainMessageBoxState == 1) {
				mainMessageBoxState = 0;

				//DelayCallbackFunction pCB = &MenuStateOptions::delayedCallbackFunctionOk;
				//delayedCallbackList.push_back(pCB);

				return true;
			}

			cegui_manager.hideMessageBox();
		}
		else if(cegui_manager.isControlMessageBoxCancel(ctl) == true) {

			CoreData &coreData				= CoreData::getInstance();
			SoundRenderer &soundRenderer	= SoundRenderer::getInstance();

			soundRenderer.playFx(coreData.getClickSoundA());

			if(mainMessageBoxState == 1) {
				mainMessageBoxState = 0;
			}

			cegui_manager.hideMessageBox();
		}
		else if(ctl == cegui_manager.getControl("ButtonReturn")) {

			DelayCallbackFunction pCB = &MenuStateCustomGame::delayedCallbackFunctionReturn;
			delayedCallbackList.push_back(pCB);

			return true;
		}
		else if(ctl == cegui_manager.getControl("ButtonReloadLastSettings")) {

			DelayCallbackFunction pCB = &MenuStateCustomGame::delayedCallbackFunctionReloadLastSettings;
			delayedCallbackList.push_back(pCB);

			return true;
		}
		else if(ctl == cegui_manager.getControl("ButtonPlay")) {

			DelayCallbackFunction pCB = &MenuStateCustomGame::delayedCallbackFunctionPlayNow;
			delayedCallbackList.push_back(pCB);

			return true;
		}
		else if(ctl == cegui_manager.getControl("ButtonClearBlockedPlayers")) {

			//printf("Clear1\n");
			CoreData &coreData				= CoreData::getInstance();
			SoundRenderer &soundRenderer	= SoundRenderer::getInstance();

			soundRenderer.playFx(coreData.getClickSoundB());

			ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
			if(serverInterface != NULL) {
				//printf("Clear2\n");

				ServerSocket *serverSocket = serverInterface->getServerSocket();
				if(serverSocket != NULL) {
					//printf("Clear3\n");
					serverSocket->clearBlockedIPAddress();
				}
			}

			return true;
		}
		else {

			for(int index = 0; index < GameConstants::maxPlayers; ++index) {
				if(ctl == cegui_manager.getControl("ButtonPlayer" + intToStr(index+1) + "Block")) {

					CoreData &coreData				= CoreData::getInstance();
					SoundRenderer &soundRenderer	= SoundRenderer::getInstance();

					soundRenderer.playFx(coreData.getClickSoundB());

					ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
					if(serverInterface != NULL) {
						if(serverInterface->getSlot(index,true) != NULL &&
						   serverInterface->getSlot(index,true)->isConnected()) {

							ServerSocket *serverSocket = serverInterface->getServerSocket();
							if(serverSocket != NULL) {
								serverSocket->addIPAddressToBlockedList(serverInterface->getSlot(index,true)->getIpAddress());

								Lang &lang= Lang::getInstance();
								const vector<string> languageList = serverInterface->getGameSettings()->getUniqueNetworkPlayerLanguages();
								for(unsigned int j = 0; j < languageList.size(); ++j) {
									char szMsg[8096]="";
									if(lang.hasString("BlockPlayerServerMsg",languageList[j]) == true) {
										snprintf(szMsg,8096,lang.getString("BlockPlayerServerMsg",languageList[j]).c_str(),serverInterface->getSlot(index,true)->getIpAddress().c_str());
									}
									else {
										snprintf(szMsg,8096,"The server has temporarily blocked IP Address [%s] from this game.",serverInterface->getSlot(index,true)->getIpAddress().c_str());
									}

									serverInterface->sendTextMessage(szMsg,-1, true,languageList[j]);
								}
								sleep(1);
								serverInterface->getSlot(index,true)->close();
							}
						}
					}

					return true;
				}
			}
		}
	}
	else if(name == cegui_manager.getEventComboboxChangeAccepted()) {

		if(ctl == cegui_manager.getControl("ComboMapFilter")) {

			MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
			MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));

			int selectedIndex = cegui_manager.getSelectedItemIndexFromComboBoxControl(cegui_manager.getControl("ComboMapFilter"));
			switchToMapGroup(selectedIndex);

			if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s\n", getCurrentMapFile().c_str());

			if(getCurrentMapFile() != "") {
				loadMapInfo(Config::getMapPath(getCurrentMapFile()), &mapInfo, true);
				cegui_manager.setControlText("LabelMapInfo",mapInfo.desc);
			}
			else {
				cegui_manager.setControlText("LabelMapInfo","");
			}

			updateControlers();
			updateNetworkSlots();

			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			bool publishToServerEnabled =
					cegui_manager.getCheckboxControlChecked(
							cegui_manager.getControl("CheckboxPublishGame"));

			if(publishToServerEnabled == true) {
				needToRepublishToMasterserver = true;
			}

			if(hasNetworkGameSettings() == true) {
				needToSetChangedGameSettings = true;
				lastSetChangedGameSettings   = time(NULL);
			}

			return true;
		}
		else if(ctl == cegui_manager.getControl("ComboMap")) {
			if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s\n", getCurrentMapFile().c_str());

			MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
			MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));

			loadMapInfo(Config::getMapPath(getCurrentMapFile(),"",false), &mapInfo, true);
			//labelMapInfo.setText(mapInfo.desc);

			cegui_manager.setControlText("LabelMapInfo",mapInfo.desc);

			updateControlers();
			updateNetworkSlots();

			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			bool publishToServerEnabled =
					cegui_manager.getCheckboxControlChecked(
							cegui_manager.getControl("CheckboxPublishGame"));

			if(publishToServerEnabled == true) {
				needToRepublishToMasterserver = true;
			}

			if(hasNetworkGameSettings() == true) {
				//delay publishing for 5 seconds
				needToPublishDelayed=true;
				mapPublishingDelayTimer=time(NULL);
			}

			return true;
		}
		else if(ctl == cegui_manager.getControl("ComboBoxTechTree")) {

			//printf("LINE: %d\n",__LINE__);

			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			int techTreeCount = cegui_manager.getItemCountInComboBoxControl(cegui_manager.getControl("ComboBoxTechTree"));

			//printf("LINE: %d\n",__LINE__);
			reloadFactions(techTreeCount <= 1,(getAllowNetworkScenario() == true ? scenarioFiles.at(getSelectedNetworkScenarioIndex()) : ""));

			//printf("LINE: %d\n",__LINE__);

			MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
			MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));

			//MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			bool publishToServerEnabled =
					cegui_manager.getCheckboxControlChecked(
							cegui_manager.getControl("CheckboxPublishGame"));

			if(publishToServerEnabled == true) {
				needToRepublishToMasterserver = true;
			}

			if(hasNetworkGameSettings() == true) {
				needToSetChangedGameSettings = true;
				lastSetChangedGameSettings   = time(NULL);
			}

			return true;
		}
		else if(ctl == cegui_manager.getControl("ComboBoxTileset")) {
			MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
			MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));

			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			bool publishToServerEnabled =
					cegui_manager.getCheckboxControlChecked(
							cegui_manager.getControl("CheckboxPublishGame"));

			if(publishToServerEnabled == true) {
				needToRepublishToMasterserver = true;
			}
			if(hasNetworkGameSettings() == true) {
				//delay publishing for 5 seconds
				needToPublishDelayed=true;
				mapPublishingDelayTimer=time(NULL);
			}

			return true;
		}
		else if(ctl == cegui_manager.getControl("ComboBoxFogOfWar")) {

			MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
			MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));

			cleanupMapPreviewTexture();

			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			bool publishToServerEnabled =
					cegui_manager.getCheckboxControlChecked(
							cegui_manager.getControl("CheckboxPublishGame"));

			if(publishToServerEnabled == true) {
				needToRepublishToMasterserver = true;
			}

			if(hasNetworkGameSettings() == true) {
				needToSetChangedGameSettings = true;
				lastSetChangedGameSettings   = time(NULL);
			}
			return true;
		}
		else if(ctl == cegui_manager.getControl("ComboHumanPlayerStatus")) {

			if(hasNetworkGameSettings() == true) {
				if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

				CoreData &coreData				= CoreData::getInstance();
				SoundRenderer &soundRenderer	= SoundRenderer::getInstance();

				soundRenderer.playFx(coreData.getClickSoundC());
				if(getNetworkPlayerStatus() == npst_PickSettings) {
					//listBoxPlayerStatus.setTextColor(Vec3f(1.0f,0.0f,0.0f));
					//listBoxPlayerStatus.setLighted(true);
				}
				else if(getNetworkPlayerStatus() == npst_BeRightBack) {
					//listBoxPlayerStatus.setTextColor(Vec3f(1.0f,1.0f,0.0f));
					//listBoxPlayerStatus.setLighted(true);
				}
				else if(getNetworkPlayerStatus() == npst_Ready) {
					//listBoxPlayerStatus.setTextColor(Vec3f(0.0f,1.0f,0.0f));
					//listBoxPlayerStatus.setLighted(false);
				}

				if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

				//MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
				bool publishToServerEnabled =
						cegui_manager.getCheckboxControlChecked(
								cegui_manager.getControl("CheckboxPublishGame"));

	            if(publishToServerEnabled == true) {
	                needToRepublishToMasterserver = true;
	            }

	            if(hasNetworkGameSettings() == true) {
	                needToSetChangedGameSettings = true;
	                lastSetChangedGameSettings   = time(NULL);
	            }
			}

			return true;
		}
		else if(ctl == cegui_manager.getControl("ComboAIAcceptPercent")) {

			MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
			MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));

			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			bool publishToServerEnabled =
					cegui_manager.getCheckboxControlChecked(
							cegui_manager.getControl("CheckboxPublishGame"));

			if(publishToServerEnabled == true) {
				needToRepublishToMasterserver = true;
			}

			if(hasNetworkGameSettings() == true) {
				needToSetChangedGameSettings = true;
				lastSetChangedGameSettings   = time(NULL);
			}

			return true;
		}
		else if(ctl == cegui_manager.getControl("ComboNetworkScenarios")) {

			processScenario();

			return true;
		}
		else {
			//printf("LINE: %d name [%s]\n",__LINE__,name.c_str());

			for(int index = 0; index < GameConstants::maxPlayers; ++index) {
				if(ctl == cegui_manager.getControl("ComboBoxPlayer" + intToStr(index+1) + "Control")) {

					//printf("LINE: %d name [%s] index: %d\n",__LINE__,name.c_str(),index);

					//ServerInterface* serverInterface = NetworkManager::getInstance().getServerInterface();
					//ConnectionSlot *slot = serverInterface->getSlot(index,true);

					int selectedControlItemIndex = cegui_manager.getSelectedItemIndexFromComboBoxControl(ctl);

//					bool checkControTypeClicked = false;
//					int selectedControlItemIndex = cegui_manager.getSelectedItemIndexFromComboBoxControl(ctl);
//					if(selectedControlItemIndex != ctNetwork ||
//						(selectedControlItemIndex == ctNetwork &&
//								(slot == NULL || slot->isConnected() == false))) {
//						checkControTypeClicked = true;
//					}

					//printf("checkControTypeClicked = %d selectedControlItemIndex = %d i = %d\n",checkControTypeClicked,selectedControlItemIndex,i);

//					if(selectedControlItemIndex != ctHuman &&
//							checkControTypeClicked == true) {
						if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

						ControlType currentControlType = static_cast<ControlType>(
								selectedControlItemIndex);
						int slotsToChangeStart = index;
						int slotsToChangeEnd = index;
						// If control is pressed while changing player types then change all other slots to same type
						if(::Shared::Platform::Window::isKeyStateModPressed(KMOD_CTRL) == true &&
								currentControlType != ctHuman) {
							slotsToChangeStart = 0;
							slotsToChangeEnd = mapInfo.players-1;
						}

						for(int slotIndex = slotsToChangeStart;
								slotIndex <= slotsToChangeEnd; ++slotIndex) {

							selectedControlItemIndex = cegui_manager.getSelectedItemIndexFromComboBoxControl(ctl);

							CEGUI::Window *slotCtl = cegui_manager.getControl(
											"ComboBoxPlayer" + intToStr(slotIndex+1) + "Control");
							int selectedSlotControlItemIndex = cegui_manager.
									getSelectedItemIndexFromComboBoxControl(slotCtl);
							if(slotIndex != index && static_cast<ControlType>(
									selectedSlotControlItemIndex) != ctHuman) {

								//listBoxControls[slotIndex].setSelectedItemIndex(listBoxControls[i].getSelectedItemIndex());
								cegui_manager.setSelectedItemInComboBoxControl(
										slotCtl,selectedControlItemIndex);
							}

							selectedSlotControlItemIndex = cegui_manager.
									getSelectedItemIndexFromComboBoxControl(slotCtl);
							// Skip over networkunassigned
							if(selectedSlotControlItemIndex == ctNetworkUnassigned &&
								selectedControlItemIndex != ctNetworkUnassigned) {
								//listBoxControls[slotIndex].mouseClick(x, y);
								cegui_manager.setSelectedItemInComboBoxControl(
										slotCtl,selectedControlItemIndex+1);
							}

							//look for human players
							int humanIndex1 = -1;
							int humanIndex2 = -1;
							for(int playerIndex = 0;
									playerIndex < GameConstants::maxPlayers; ++playerIndex) {

								CEGUI::Window *slotCtlPlayer = cegui_manager.getControl(
												"ComboBoxPlayer" + intToStr(playerIndex+1) + "Control");

								int selectedSlotControlPlayerItemIndex = cegui_manager.
										getSelectedItemIndexFromComboBoxControl(slotCtlPlayer);

								ControlType ct = static_cast<ControlType>(selectedSlotControlPlayerItemIndex);
								if(ct == ctHuman) {
									if(humanIndex1 == -1) {
										humanIndex1 = playerIndex;
									}
									else {
										humanIndex2 = playerIndex;
									}
								}
							}

							if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d] humanIndex1 = %d, humanIndex2 = %d\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,humanIndex1,humanIndex2);

							//printf("Line: %d slotIndex: %d selectedControlItemIndex: %d humanIndex1: %d humanIndex2: %d\n",__LINE__,slotIndex,selectedControlItemIndex,humanIndex1,humanIndex2);

							//no human
							if(humanIndex1 == -1 && humanIndex2 == -1) {
								setSlotHuman(index);
								if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d] i = %d, labelPlayerNames[i].getText() [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,slotIndex,getPlayerNameText(index).c_str());

								//printf("humanIndex1 = %d humanIndex2 = %d i = %d listBoxControls[i].getSelectedItemIndex() = %d\n",humanIndex1,humanIndex2,i,listBoxControls[i].getSelectedItemIndex());
							}
							//2 humans
							else if(humanIndex1 != -1 && humanIndex2 != -1) {
								int closeSlotIndex = (humanIndex1 == slotIndex ? humanIndex2: humanIndex1);
								int humanSlotIndex = (closeSlotIndex == humanIndex1 ? humanIndex2 : humanIndex1);

								string origPlayName = getPlayerNameText(closeSlotIndex);

								if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d] closeSlotIndex = %d, origPlayName [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,closeSlotIndex,origPlayName.c_str());

								//listBoxControls[closeSlotIndex].setSelectedItemIndex(ctClosed);
								CEGUI::Window *slotCtlClose = cegui_manager.getControl(
												"ComboBoxPlayer" + intToStr(closeSlotIndex+1) + "Control");

								cegui_manager.setSelectedItemInComboBoxControl(slotCtlClose,ctClosed);

								//printf("Line: %d closeSlotIndex: %d\n",__LINE__,closeSlotIndex);

								setSlotHuman(humanSlotIndex);
								setPlayerNameText(humanSlotIndex,(origPlayName != "" ? origPlayName : getHumanPlayerName()));
							}
							updateNetworkSlots();

							MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
							bool publishToServerEnabled =
									cegui_manager.getCheckboxControlChecked(
											cegui_manager.getControl("CheckboxPublishGame"));

							if(publishToServerEnabled == true) {
								needToRepublishToMasterserver = true;
							}

							if(hasNetworkGameSettings() == true) {
								needToSetChangedGameSettings = true;
								lastSetChangedGameSettings   = time(NULL);
							}

							//printf("LINE: %d name [%s] index: %d slotIndex: %d\n",__LINE__,name.c_str(),index,slotIndex);

							updateResourceMultiplier(slotIndex);

							//printf("LINE: %d name [%s] index: %d slotIndex: %d\n",__LINE__,name.c_str(),index,slotIndex);
						}
					//}

					return true;
				}
				else if(ctl == cegui_manager.getControl("ComboBoxPlayer" + intToStr(index+1) + "Faction")) {

					int selectedControlItemIndex = cegui_manager.getSelectedItemIndexFromComboBoxControl(ctl);

					//printf("selectedControlItemIndex = %d\n",selectedControlItemIndex);
					//printf("factionFiles[selectedControlItemIndex] = %s\n",factionFiles[selectedControlItemIndex].c_str());

					// Disallow CPU players to be observers
					if(factionFiles.at(selectedControlItemIndex) == formatString(GameConstants::OBSERVER_SLOTNAME) &&
						(getSelectedPlayerControlTypeIndex(index) == ctCpuEasy ||
						 getSelectedPlayerControlTypeIndex(index) == ctCpu ||
						 getSelectedPlayerControlTypeIndex(index) == ctCpuUltra ||
						 getSelectedPlayerControlTypeIndex(index) == ctCpuMega)) {

						//listBoxFactions[i].setSelectedItemIndex(0);
						cegui_manager.setSelectedItemInComboBoxControl(ctl,0);
					}
					//

					MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
					bool publishToServerEnabled =
							cegui_manager.getCheckboxControlChecked(
									cegui_manager.getControl("CheckboxPublishGame"));

					if(publishToServerEnabled == true) {
						needToRepublishToMasterserver = true;
					}

					if(hasNetworkGameSettings() == true) {
						needToSetChangedGameSettings = true;
						lastSetChangedGameSettings   = time(NULL);
					}

					return true;
				}
				else if(ctl == cegui_manager.getControl("ComboBoxPlayer" + intToStr(index+1) + "Team")) {

					int selectedTeamItemIndex = cegui_manager.getSelectedItemIndexFromComboBoxControl(ctl);

					int selectedControlItemIndex = getSelectedPlayerFactionTypeIndex(index);
					if(factionFiles.at(selectedControlItemIndex) != formatString(GameConstants::OBSERVER_SLOTNAME)) {
						if(selectedTeamItemIndex + 1 != (GameConstants::maxPlayers + fpt_Observer)) {
							lastSelectedTeamIndex[index] = selectedTeamItemIndex;
						}
					}
					else {
						lastSelectedTeamIndex[index] = -1;
					}

					MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
					bool publishToServerEnabled =
							cegui_manager.getCheckboxControlChecked(
									cegui_manager.getControl("CheckboxPublishGame"));

					if(publishToServerEnabled == true) {
						needToRepublishToMasterserver = true;
					}

					if(hasNetworkGameSettings() == true) {
						needToSetChangedGameSettings = true;
						lastSetChangedGameSettings   = time(NULL);;
					}

					return true;
				}
			}
		}
	}
	else if(name == cegui_manager.getEventCheckboxClicked()) {

		if(ctl == cegui_manager.getControl("CheckboxPublishGame")) {

			MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
			MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));

			needToRepublishToMasterserver = true;

			CoreData &coreData				= CoreData::getInstance();
			SoundRenderer &soundRenderer	= SoundRenderer::getInstance();

			soundRenderer.playFx(coreData.getClickSoundC());

			bool publishToServerEnabled =
					cegui_manager.getCheckboxControlChecked(
							cegui_manager.getControl("CheckboxPublishGame"));
			ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
			serverInterface->setPublishEnabled(publishToServerEnabled);

			return true;
		}
		else if(ctl == cegui_manager.getControl("CheckboxAllowSwitchTeams")) {

			MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
			MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));

			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			bool publishToServerEnabled =
					cegui_manager.getCheckboxControlChecked(
							cegui_manager.getControl("CheckboxPublishGame"));

			if(publishToServerEnabled == true) {
				needToRepublishToMasterserver = true;
			}

			if(hasNetworkGameSettings() == true) {
				needToSetChangedGameSettings = true;
				lastSetChangedGameSettings   = time(NULL);
			}

			return true;
		}
		else if(ctl == cegui_manager.getControl("CheckboxAllowObservers")) {

			MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
			MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));

			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			bool publishToServerEnabled =
					cegui_manager.getCheckboxControlChecked(
							cegui_manager.getControl("CheckboxPublishGame"));

			if(publishToServerEnabled == true) {
				needToRepublishToMasterserver = true;
			}

			reloadFactions(true,(getAllowNetworkScenario() == true ? scenarioFiles.at(getSelectedNetworkScenarioIndex()) : ""));

			if(hasNetworkGameSettings() == true) {
				needToSetChangedGameSettings = true;
				lastSetChangedGameSettings   = time(NULL);
			}

			return true;
		}
		else if(ctl == cegui_manager.getControl("CheckboxAllowInProgressJoinGame")) {

			MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
			MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));

			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			bool publishToServerEnabled =
					cegui_manager.getCheckboxControlChecked(
							cegui_manager.getControl("CheckboxPublishGame"));

			if(publishToServerEnabled == true) {
				needToRepublishToMasterserver = true;
			}

			if(hasNetworkGameSettings() == true) {
				needToSetChangedGameSettings = true;
				lastSetChangedGameSettings   = time(NULL);
			}

			ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
			serverInterface->setAllowInGameConnections(getAllowInGameJoinPlayer() == true);

			return true;
		}
		else if(ctl == cegui_manager.getControl("CheckboxAllowSharedTeamUnits")) {

			MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
			MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));

			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			bool publishToServerEnabled =
					cegui_manager.getCheckboxControlChecked(
							cegui_manager.getControl("CheckboxPublishGame"));

			if(publishToServerEnabled == true) {
				needToRepublishToMasterserver = true;
			}

			if(hasNetworkGameSettings() == true) {
				needToSetChangedGameSettings = true;
				lastSetChangedGameSettings   = time(NULL);
			}

			return true;
		}
		else if(ctl == cegui_manager.getControl("CheckboxAllowSharedTeamResources")) {

			MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
			MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));

			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			bool publishToServerEnabled =
					cegui_manager.getCheckboxControlChecked(
							cegui_manager.getControl("CheckboxPublishGame"));

			if(publishToServerEnabled == true) {
				needToRepublishToMasterserver = true;
			}

			if(hasNetworkGameSettings() == true) {
				needToSetChangedGameSettings = true;
				lastSetChangedGameSettings   = time(NULL);
			}

			return true;
		}
		else if(ctl == cegui_manager.getControl("CheckboxTechTreeTranslated")) {

			MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
			MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));

			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			bool publishToServerEnabled =
					cegui_manager.getCheckboxControlChecked(
							cegui_manager.getControl("CheckboxPublishGame"));

			if(publishToServerEnabled == true) {
				needToRepublishToMasterserver = true;
			}

			if(hasNetworkGameSettings() == true) {
				needToSetChangedGameSettings = true;
				lastSetChangedGameSettings   = time(NULL);
			}

			return true;
		}
		else if(ctl == cegui_manager.getControl("CheckboxPauseLaggingClients")) {

			MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
			MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));

			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			bool publishToServerEnabled =
					cegui_manager.getCheckboxControlChecked(
							cegui_manager.getControl("CheckboxPublishGame"));

			if(publishToServerEnabled == true) {
				needToRepublishToMasterserver = true;
			}
			if(hasNetworkGameSettings() == true) {
				needToSetChangedGameSettings = true;
				lastSetChangedGameSettings   = time(NULL);
			}

			CoreData &coreData				= CoreData::getInstance();
			SoundRenderer &soundRenderer	= SoundRenderer::getInstance();

			soundRenderer.playFx(coreData.getClickSoundC());

			return true;
		}
		else if(ctl == cegui_manager.getControl("CheckboxShowNetworkScenarios")) {

			setNetworkScenarioVisible(getAllowNetworkScenario());

			return true;
		}
	}
	else if(name == cegui_manager.getEventSpinnerValueChanged()) {
		if(ctl == cegui_manager.getControl("SpinnerAIReplaceMultiplier")) {

			MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
			MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));

			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			bool publishToServerEnabled =
					cegui_manager.getCheckboxControlChecked(
							cegui_manager.getControl("CheckboxPublishGame"));

			if(publishToServerEnabled == true) {
				needToRepublishToMasterserver = true;
			}

			if(hasNetworkGameSettings() == true) {
				needToSetChangedGameSettings = true;
				lastSetChangedGameSettings   = time(NULL);
			}

			return true;
		}
	}

	return false;
}

void MenuStateCustomGame::cleanupThread(SimpleTaskThread **thread) {
	//printf("LINE: %d *thread = %p\n",__LINE__,*thread);

    if(thread != NULL && *thread != NULL) {
    	if(SystemFlags::VERBOSE_MODE_ENABLED) printf("\n\n#1 cleanupThread callingThread [%p]\n",*thread);

    	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

    	SimpleTaskThread *threadPtr = *thread;
    	int value = threadPtr->getUserdataAsInt();
    	THREAD_NOTIFIER_TYPE threadType = (THREAD_NOTIFIER_TYPE)value;

    	if(SystemFlags::VERBOSE_MODE_ENABLED) printf("\n\n#1. cleanupThread callingThread [%p] value = %d\n",*thread,value);

        needToBroadcastServerSettings = false;
        needToRepublishToMasterserver = false;
        lastNetworkPing               = time(NULL);
        threadPtr->setThreadOwnerValid(false);

        if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

        if(SystemFlags::VERBOSE_MODE_ENABLED) printf("\n\n#1.. cleanupThread callingThread [%p] value = %d\n",*thread,value);

        if(forceWaitForShutdown == true) {
    		time_t elapsed = time(NULL);
    		threadPtr->signalQuit();

    		if(SystemFlags::VERBOSE_MODE_ENABLED) printf("\n\n#1a cleanupThread callingThread [%p]\n",*thread);

    		for(;(threadPtr->canShutdown(false) == false  ||
    				threadPtr->getRunningStatus() == true) &&
    			difftime((long int)time(NULL),elapsed) <= 15;) {
    			//sleep(150);
    		}

    		if(SystemFlags::VERBOSE_MODE_ENABLED) printf("\n\n#1b cleanupThread callingThread [%p]\n",*thread);

    		if(threadPtr->canShutdown(true) == true &&
    				threadPtr->getRunningStatus() == false) {
    			if(SystemFlags::VERBOSE_MODE_ENABLED) printf("\n\n#1c cleanupThread callingThread [%p]\n",*thread);

    			delete threadPtr;
    			//printf("LINE: %d *thread = %p\n",__LINE__,*thread);
    			if(SystemFlags::VERBOSE_MODE_ENABLED) printf("In [%s::%s Line: %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
    		}
    		else {
    			if(SystemFlags::VERBOSE_MODE_ENABLED) printf("\n\n#1d cleanupThread callingThread [%p]\n",*thread);

    			char szBuf[8096]="";
    			snprintf(szBuf,8096,"In [%s::%s %d] Error cannot shutdown thread\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
    			//SystemFlags::OutputDebug(SystemFlags::debugError,szBuf);
    			if(SystemFlags::VERBOSE_MODE_ENABLED) printf("%s",szBuf);
    			if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s",szBuf);

    			if(threadType == tnt_MASTERSERVER) {
    				threadPtr->setOverrideShutdownTask(shutdownTaskStatic);
    			}
    			threadPtr->setDeleteSelfOnExecutionDone(true);
    			threadPtr->setDeleteAfterExecute(true);
    			//printf("LINE: %d *thread = %p\n",__LINE__,*thread);
    		}
    		threadPtr = NULL;
    		//if(SystemFlags::VERBOSE_MODE_ENABLED) printf("\n\n#1e cleanupThread callingThread [%p]\n",*thread);
        }
        else {
        	if(SystemFlags::VERBOSE_MODE_ENABLED) printf("\n\n#1f cleanupThread callingThread [%p]\n",*thread);
        	threadPtr->signalQuit();
        	sleep(0);
        	if(threadPtr->canShutdown(true) == true &&
        			threadPtr->getRunningStatus() == false) {
        		if(SystemFlags::VERBOSE_MODE_ENABLED) printf("\n\n#1g cleanupThread callingThread [%p]\n",*thread);
        		delete threadPtr;
        		//printf("LINE: %d *thread = %p\n",__LINE__,*thread);
        	}
    		else {
    			if(SystemFlags::VERBOSE_MODE_ENABLED) printf("\n\n#1h cleanupThread callingThread [%p]\n",*thread);
    			char szBuf[8096]="";
    			snprintf(szBuf,8096,"In [%s::%s %d] Error cannot shutdown thread\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
    			//SystemFlags::OutputDebug(SystemFlags::debugError,szBuf);
    			if(SystemFlags::VERBOSE_MODE_ENABLED) printf("%s",szBuf);
    			if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s",szBuf);

    			if(threadType == tnt_MASTERSERVER) {
    				threadPtr->setOverrideShutdownTask(shutdownTaskStatic);
    			}
    			threadPtr->setDeleteSelfOnExecutionDone(true);
    			threadPtr->setDeleteAfterExecute(true);
    			//printf("LINE: %d *thread = %p\n",__LINE__,*thread);
    		}
        }

        *thread = NULL;
        if(SystemFlags::VERBOSE_MODE_ENABLED) printf("\n\n#2 cleanupThread callingThread [%p]\n",*thread);
    }
    //printf("LINE: %d *thread = %p\n",__LINE__,*thread);
}

void MenuStateCustomGame::cleanup() {
	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	if(publishToMasterserverThread) {
		//printf("LINE: %d\n",__LINE__);
		cleanupThread(&publishToMasterserverThread);
	}
	if(publishToClientsThread) {
		//printf("LINE: %d\n",__LINE__);
		cleanupThread(&publishToClientsThread);
	}

	//printf("LINE: %d\n",__LINE__);

    if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	cleanupMapPreviewTexture();

	if(factionVideo != NULL) {
		factionVideo->closePlayer();
		delete factionVideo;
		factionVideo = NULL;
	}

	if(forceWaitForShutdown == true) {
		NetworkManager::getInstance().end();
	}

	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	cegui_manager.unsubscribeEvents(this->containerName);

	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
}

MenuStateCustomGame::~MenuStateCustomGame() {
	if(SystemFlags::VERBOSE_MODE_ENABLED) printf("In [%s::%s Line: %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

    cleanup();

    if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
	if(SystemFlags::VERBOSE_MODE_ENABLED) printf("In [%s::%s Line: %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
}

void MenuStateCustomGame::returnToParentMenu() {
	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	needToBroadcastServerSettings = false;
	needToRepublishToMasterserver = false;
	lastNetworkPing               = time(NULL);
	ParentMenuState parentMenuState = this->parentMenuState;

	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	forceWaitForShutdown = false;
	if(parentMenuState==pMasterServer) {
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
		cleanup();
		mainMenu->setState(new MenuStateMasterserver(program, mainMenu));
	}
	else if(parentMenuState==pLanGame) {
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
		cleanup();
		mainMenu->setState(new MenuStateJoinGame(program, mainMenu));
	}
	else {
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
		cleanup();
		mainMenu->setState(new MenuStateNewGame(program, mainMenu));
	}

	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
}

void MenuStateCustomGame::mouseClick(int x, int y, MouseButton mouseButton) {
	if(isMasterserverMode() == true) {
		return;
	}

    try {
        //CoreData &coreData= CoreData::getInstance();
        //SoundRenderer &soundRenderer= SoundRenderer::getInstance();
        //int oldListBoxMapfilterIndex=listBoxMapFilter.getSelectedItemIndex();

//        if(mainMessageBox.getEnabled()){
//            int button= 0;
//            if(mainMessageBox.mouseClick(x, y, button)) {
//                soundRenderer.playFx(coreData.getClickSoundA());
//                if(button == 0) {
//                    mainMessageBox.setEnabled(false);
//                }
//            }
//        }
//        else {
        	string advanceToItemStartingWith = "";
        	if(::Shared::Platform::Window::isKeyStateModPressed(KMOD_SHIFT) == true) {
        		const wchar_t lastKey = ::Shared::Platform::Window::extractLastKeyPressed();
//        		xxx:
//        		string hehe=lastKey;
//        		printf("lastKey = %d [%c] '%s'\n",lastKey,lastKey,hehe);
        		advanceToItemStartingWith =  lastKey;
        	}

        	if(mapPreviewTexture != NULL) {
//        		printf("X: %d Y: %d      [%d, %d, %d, %d]\n",
//        				x, y,
//        				this->render_mapPreviewTexture_X, this->render_mapPreviewTexture_X + this->render_mapPreviewTexture_W,
//        				this->render_mapPreviewTexture_Y, this->render_mapPreviewTexture_Y + this->render_mapPreviewTexture_H);

				if( x >= this->render_mapPreviewTexture_X && x <= this->render_mapPreviewTexture_X + this->render_mapPreviewTexture_W &&
					y >= this->render_mapPreviewTexture_Y && y <= this->render_mapPreviewTexture_Y + this->render_mapPreviewTexture_H) {

					if( this->render_mapPreviewTexture_X == mapPreviewTexture_X &&
						this->render_mapPreviewTexture_Y == mapPreviewTexture_Y &&
						this->render_mapPreviewTexture_W == mapPreviewTexture_W &&
						this->render_mapPreviewTexture_H == mapPreviewTexture_H) {

						const Metrics &metrics= Metrics::getInstance();

						this->render_mapPreviewTexture_X = 0;
						this->render_mapPreviewTexture_Y = 0;
						this->render_mapPreviewTexture_W = metrics.getVirtualW();
						this->render_mapPreviewTexture_H = metrics.getVirtualH();
						this->zoomedMap = true;

						cleanupMapPreviewTexture();
					}
					else {
						this->render_mapPreviewTexture_X = mapPreviewTexture_X;
						this->render_mapPreviewTexture_Y = mapPreviewTexture_Y;
						this->render_mapPreviewTexture_W = mapPreviewTexture_W;
						this->render_mapPreviewTexture_H = mapPreviewTexture_H;
						this->zoomedMap = false;

						cleanupMapPreviewTexture();
					}
					return;
				}
	        	if(this->zoomedMap == true){
	        		return;
	        	}
        	}

//        	if(activeInputLabel!=NULL && !(activeInputLabel->mouseClick(x,y))){
//				setActiveInputLabel(NULL);
//			}
//			if(buttonReturn.mouseClick(x,y) || serverInitError == true) {
//				if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
//
//				soundRenderer.playFx(coreData.getClickSoundA());
//
//				MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//				MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//				needToBroadcastServerSettings = false;
//				needToRepublishToMasterserver = false;
//				lastNetworkPing               = time(NULL);
//				safeMutex.ReleaseLock();
//				safeMutexCLI.ReleaseLock();
//
//				if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
//
//				returnToParentMenu();
//				return;
//			}
//			else if(buttonPlayNow.mouseClick(x,y) && buttonPlayNow.getEnabled()) {
//				if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
//
//				PlayNow(true);
//				return;
//			}
//			else if(buttonRestoreLastSettings.mouseClick(x,y) && buttonRestoreLastSettings.getEnabled()) {
//				soundRenderer.playFx(coreData.getClickSoundB());
//
//				RestoreLastGameSettings();
//			}
//			else if(listBoxMap.mouseClick(x, y,advanceToItemStartingWith)){
//				if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s\n", getCurrentMapFile().c_str());
//
//				MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//				MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//
//				loadMapInfo(Config::getMapPath(getCurrentMapFile(),"",false), &mapInfo, true);
//				//labelMapInfo.setText(mapInfo.desc);
//
//				MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
//				cegui_manager.setControlText("LabelMapInfo",mapInfo.desc);
//
//				updateControlers();
//				updateNetworkSlots();
//
//				if(checkBoxPublishServer.getValue() == true) {
//					needToRepublishToMasterserver = true;
//				}
//
//				if(hasNetworkGameSettings() == true) {
//					//delay publishing for 5 seconds
//					needToPublishDelayed=true;
//					mapPublishingDelayTimer=time(NULL);
//				}
//			}
//			else if (checkBoxAdvanced.getValue() == 1 && listBoxFogOfWar.mouseClick(x, y)) {
//				MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//				MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//
//				cleanupMapPreviewTexture();
//				if(checkBoxPublishServer.getValue() == true) {
//					needToRepublishToMasterserver = true;
//				}
//
//				if(hasNetworkGameSettings() == true) {
//					needToSetChangedGameSettings = true;
//					lastSetChangedGameSettings   = time(NULL);
//				}
//			}
//			else if (checkBoxAdvanced.getValue() == 1 && checkBoxAllowObservers.mouseClick(x, y)) {
//				MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//				MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//
//				MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
//				bool publishToServerEnabled =
//						cegui_manager.getCheckboxControlChecked(
//								cegui_manager.getControl("CheckboxPublishGame"));
//
//				if(publishToServerEnabled == true) {
//					needToRepublishToMasterserver = true;
//				}
//
//				reloadFactions(true,(checkBoxScenario.getValue() == true ? scenarioFiles[listBoxScenario.getSelectedItemIndex()] : ""));
//
//				if(hasNetworkGameSettings() == true) {
//					needToSetChangedGameSettings = true;
//					lastSetChangedGameSettings   = time(NULL);
//				}
//			}
//			else if (checkBoxAllowInGameJoinPlayer.mouseClick(x, y)) {
//				MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//				MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//
//				MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
//				bool publishToServerEnabled =
//						cegui_manager.getCheckboxControlChecked(
//								cegui_manager.getControl("CheckboxPublishGame"));
//
//				if(publishToServerEnabled == true) {
//					needToRepublishToMasterserver = true;
//				}
//
//				if(hasNetworkGameSettings() == true) {
//					needToSetChangedGameSettings = true;
//					lastSetChangedGameSettings   = time(NULL);
//				}
//
//				ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
//				serverInterface->setAllowInGameConnections(checkBoxAllowInGameJoinPlayer.getValue() == true);
//			}
//			else if (checkBoxAdvanced.getValue() == 1 && checkBoxAllowTeamUnitSharing.mouseClick(x, y)) {
//				MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//				MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//
//				MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
//				bool publishToServerEnabled =
//						cegui_manager.getCheckboxControlChecked(
//								cegui_manager.getControl("CheckboxPublishGame"));
//
//				if(publishToServerEnabled == true) {
//					needToRepublishToMasterserver = true;
//				}
//
//				if(hasNetworkGameSettings() == true) {
//					needToSetChangedGameSettings = true;
//					lastSetChangedGameSettings   = time(NULL);
//				}
//			}
//			else if (checkBoxAdvanced.getValue() == 1 && checkBoxAllowTeamResourceSharing.mouseClick(x, y)) {
//				MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//				MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//
//				MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
//				bool publishToServerEnabled =
//						cegui_manager.getCheckboxControlChecked(
//								cegui_manager.getControl("CheckboxPublishGame"));
//
//				if(publishToServerEnabled == true) {
//					needToRepublishToMasterserver = true;
//				}
//
//				if(hasNetworkGameSettings() == true) {
//					needToSetChangedGameSettings = true;
//					lastSetChangedGameSettings   = time(NULL);
//				}
//			}
//			else if (checkBoxAllowNativeLanguageTechtree.mouseClick(x, y)) {
//				MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//				MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//
//				MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
//				bool publishToServerEnabled =
//						cegui_manager.getCheckboxControlChecked(
//								cegui_manager.getControl("CheckboxPublishGame"));
//
//				if(publishToServerEnabled == true) {
//					needToRepublishToMasterserver = true;
//				}
//
//				if(hasNetworkGameSettings() == true)
//				{
//					needToSetChangedGameSettings = true;
//					lastSetChangedGameSettings   = time(NULL);
//				}
//			}
//			else if (checkBoxAdvanced.getValue() == 1 && checkBoxEnableSwitchTeamMode.mouseClick(x, y)) {
//				MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//				MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//
//				MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
//				bool publishToServerEnabled =
//						cegui_manager.getCheckboxControlChecked(
//								cegui_manager.getControl("CheckboxPublishGame"));
//
//				if(publishToServerEnabled == true) {
//					needToRepublishToMasterserver = true;
//				}
//
//				if(hasNetworkGameSettings() == true)
//				{
//					needToSetChangedGameSettings = true;
//					lastSetChangedGameSettings   = time(NULL);
//				}
//			}
//			else if (checkBoxAdvanced.getValue() == 1 && listBoxAISwitchTeamAcceptPercent.getEnabled() && listBoxAISwitchTeamAcceptPercent.mouseClick(x, y)) {
//				MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//				MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//
//				MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
//				bool publishToServerEnabled =
//						cegui_manager.getCheckboxControlChecked(
//								cegui_manager.getControl("CheckboxPublishGame"));
//
//				if(publishToServerEnabled == true) {
//					needToRepublishToMasterserver = true;
//				}
//
//				if(hasNetworkGameSettings() == true)
//				{
//					needToSetChangedGameSettings = true;
//					lastSetChangedGameSettings   = time(NULL);
//				}
//			}
//			else if (checkBoxAdvanced.getValue() == 1 && listBoxFallbackCpuMultiplier.getEditable() == true && listBoxFallbackCpuMultiplier.mouseClick(x, y)) {
//				MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//				MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//
//				MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
//				bool publishToServerEnabled =
//						cegui_manager.getCheckboxControlChecked(
//								cegui_manager.getControl("CheckboxPublishGame"));
//
//				if(publishToServerEnabled == true) {
//					needToRepublishToMasterserver = true;
//				}
//
//				if(hasNetworkGameSettings() == true)
//				{
//					needToSetChangedGameSettings = true;
//					lastSetChangedGameSettings   = time(NULL);
//				}
//			}
			//else if (checkBoxAdvanced.mouseClick(x, y)) {
			//}
//			else if(listBoxTileset.mouseClick(x, y,advanceToItemStartingWith)) {
//				MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//				MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//
//				if(checkBoxPublishServer.getValue() == true) {
//					needToRepublishToMasterserver = true;
//				}
//				if(hasNetworkGameSettings() == true)
//				{
//
//					//delay publishing for 5 seconds
//					needToPublishDelayed=true;
//					mapPublishingDelayTimer=time(NULL);
//				}
//			}
//			else if(listBoxMapFilter.mouseClick(x, y)){
//				MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//				MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//
//				switchToNextMapGroup(listBoxMapFilter.getSelectedItemIndex()-oldListBoxMapfilterIndex);
//
//				if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s\n", getCurrentMapFile().c_str());
//
//				loadMapInfo(Config::getMapPath(getCurrentMapFile()), &mapInfo, true);
//				//labelMapInfo.setText(mapInfo.desc);
//
//				MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
//				cegui_manager.setControlText("LabelMapInfo",mapInfo.desc);
//
//				updateControlers();
//				updateNetworkSlots();
//
//				if(checkBoxPublishServer.getValue() == true) {
//					needToRepublishToMasterserver = true;
//				}
//
//				if(hasNetworkGameSettings() == true)
//				{
//					needToSetChangedGameSettings = true;
//					lastSetChangedGameSettings   = time(NULL);
//				}
//			}
//			else if(listBoxTechTree.mouseClick(x, y,advanceToItemStartingWith)){
//				reloadFactions(listBoxTechTree.getItemCount() <= 1,(checkBoxScenario.getValue() == true ? scenarioFiles[listBoxScenario.getSelectedItemIndex()] : ""));
//
//				MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//				MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//
//				if(checkBoxPublishServer.getValue() == true) {
//					needToRepublishToMasterserver = true;
//				}
//
//				if(hasNetworkGameSettings() == true)
//				{
//					needToSetChangedGameSettings = true;
//					lastSetChangedGameSettings   = time(NULL);
//				}
//			}
//			else if(checkBoxPublishServer.mouseClick(x, y) && checkBoxPublishServer.getEditable()) {
//				MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//				MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//
//				needToRepublishToMasterserver = true;
//				soundRenderer.playFx(coreData.getClickSoundC());
//
//				ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
//				serverInterface->setPublishEnabled(checkBoxPublishServer.getValue() == true);
//			}
//			else if(labelGameName.mouseClick(x, y) && checkBoxPublishServer.getEditable()){
//				setActiveInputLabel(&labelGameName);
//			}
//			else if(checkBoxAdvanced.getValue() == 1 && checkBoxNetworkPauseGameForLaggedClients.mouseClick(x, y)) {
//				MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//				MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//
//				MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
//				bool publishToServerEnabled =
//						cegui_manager.getCheckboxControlChecked(
//								cegui_manager.getControl("CheckboxPublishGame"));
//
//				if(publishToServerEnabled == true) {
//					needToRepublishToMasterserver = true;
//				}
//				if(hasNetworkGameSettings() == true)
//				{
//					needToSetChangedGameSettings = true;
//					lastSetChangedGameSettings   = time(NULL);
//				}
//
//				soundRenderer.playFx(coreData.getClickSoundC());
//			}
//		    else if(listBoxScenario.mouseClick(x, y) || checkBoxScenario.mouseClick(x,y)) {
//		        processScenario();
//			}
			else {
				for(int i = 0; i < mapInfo.players; ++i) {
					MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
					MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));

					// set multiplier
					//if(listBoxRMultiplier[i].mouseClick(x, y)) {
						//printf("Line: %d multiplier index: %d i: %d itemcount: %d\n",__LINE__,listBoxRMultiplier[i].getSelectedItemIndex(),i,listBoxRMultiplier[i].getItemCount());

						//for(int indexData = 0; indexData < listBoxRMultiplier[i].getItemCount(); ++indexData) {
							//string item = listBoxRMultiplier[i].getItem(indexData);

							//printf("Item index: %d value: %s\n",indexData,item.c_str());
						//}
					//}

					//ensure thet only 1 human player is present
					ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
					ConnectionSlot *slot = serverInterface->getSlot(i,true);

					bool checkControTypeClicked = false;
					int selectedControlItemIndex = getSelectedPlayerControlTypeIndex(i);
					if(selectedControlItemIndex != ctNetwork ||
						(selectedControlItemIndex == ctNetwork && (slot == NULL || slot->isConnected() == false))) {
						checkControTypeClicked = true;
					}

					//printf("checkControTypeClicked = %d selectedControlItemIndex = %d i = %d\n",checkControTypeClicked,selectedControlItemIndex,i);

//					if(selectedControlItemIndex != ctHuman &&
//							checkControTypeClicked == true &&
//							listBoxControls[i].mouseClick(x, y)) {
//						if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
//
//						ControlType currentControlType = static_cast<ControlType>(
//								listBoxControls[i].getSelectedItemIndex());
//						int slotsToChangeStart = i;
//						int slotsToChangeEnd = i;
//						// If control is pressed while changing player types then change all other slots to same type
//						if(::Shared::Platform::Window::isKeyStateModPressed(KMOD_CTRL) == true &&
//								currentControlType != ctHuman) {
//							slotsToChangeStart = 0;
//							slotsToChangeEnd = mapInfo.players-1;
//						}
//
//						for(int index = slotsToChangeStart; index <= slotsToChangeEnd; ++index) {
//							if(index != i && static_cast<ControlType>(
//									listBoxControls[index].getSelectedItemIndex()) != ctHuman) {
//								listBoxControls[index].setSelectedItemIndex(listBoxControls[i].getSelectedItemIndex());
//							}
//							// Skip over networkunassigned
//							if(listBoxControls[index].getSelectedItemIndex() == ctNetworkUnassigned &&
//								selectedControlItemIndex != ctNetworkUnassigned) {
//								listBoxControls[index].mouseClick(x, y);
//							}
//
//							//look for human players
//							int humanIndex1= -1;
//							int humanIndex2= -1;
//							for(int j = 0; j < GameConstants::maxPlayers; ++j) {
//								ControlType ct= static_cast<ControlType>(listBoxControls[j].getSelectedItemIndex());
//								if(ct == ctHuman) {
//									if(humanIndex1 == -1) {
//										humanIndex1= j;
//									}
//									else {
//										humanIndex2= j;
//									}
//								}
//							}
//
//							if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d] humanIndex1 = %d, humanIndex2 = %d\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,humanIndex1,humanIndex2);
//
//							//no human
//							if(humanIndex1 == -1 && humanIndex2 == -1) {
//								setSlotHuman(index);
//								if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d] i = %d, labelPlayerNames[i].getText() [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,index,labelPlayerNames[index].getText().c_str());
//
//								//printf("humanIndex1 = %d humanIndex2 = %d i = %d listBoxControls[i].getSelectedItemIndex() = %d\n",humanIndex1,humanIndex2,i,listBoxControls[i].getSelectedItemIndex());
//							}
//							//2 humans
//							else if(humanIndex1 != -1 && humanIndex2 != -1) {
//								int closeSlotIndex = (humanIndex1 == index ? humanIndex2: humanIndex1);
//								int humanSlotIndex = (closeSlotIndex == humanIndex1 ? humanIndex2 : humanIndex1);
//
//								string origPlayName = labelPlayerNames[closeSlotIndex].getText();
//
//								if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d] closeSlotIndex = %d, origPlayName [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,closeSlotIndex,origPlayName.c_str());
//
//								listBoxControls[closeSlotIndex].setSelectedItemIndex(ctClosed);
//								setSlotHuman(humanSlotIndex);
//								labelPlayerNames[humanSlotIndex].setText((origPlayName != "" ? origPlayName : getHumanPlayerName()));
//							}
//							updateNetworkSlots();
//
//							MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
//							bool publishToServerEnabled =
//									cegui_manager.getCheckboxControlChecked(
//											cegui_manager.getControl("CheckboxPublishGame"));
//
//							if(publishToServerEnabled == true) {
//								needToRepublishToMasterserver = true;
//							}
//
//							if(hasNetworkGameSettings() == true) {
//								needToSetChangedGameSettings = true;
//								lastSetChangedGameSettings   = time(NULL);
//							}
//							updateResourceMultiplier(index);
//						}
//					}
//					if(buttonClearBlockedPlayers.mouseClick(x, y)) {
//						soundRenderer.playFx(coreData.getClickSoundB());
//
//						ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
//						if(serverInterface != NULL) {
//							ServerSocket *serverSocket = serverInterface->getServerSocket();
//							if(serverSocket != NULL) {
//								serverSocket->clearBlockedIPAddress();
//							}
//						}
//					}
//					else if(buttonBlockPlayers[i].mouseClick(x, y)) {
//						soundRenderer.playFx(coreData.getClickSoundB());
//
//						ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
//						if(serverInterface != NULL) {
//							if(serverInterface->getSlot(i,true) != NULL &&
//							   serverInterface->getSlot(i,true)->isConnected()) {
//
//								ServerSocket *serverSocket = serverInterface->getServerSocket();
//								if(serverSocket != NULL) {
//									serverSocket->addIPAddressToBlockedList(serverInterface->getSlot(i,true)->getIpAddress());
//
//									Lang &lang= Lang::getInstance();
//									const vector<string> languageList = serverInterface->getGameSettings()->getUniqueNetworkPlayerLanguages();
//									for(unsigned int j = 0; j < languageList.size(); ++j) {
//										char szMsg[8096]="";
//										if(lang.hasString("BlockPlayerServerMsg",languageList[j]) == true) {
//											snprintf(szMsg,8096,lang.getString("BlockPlayerServerMsg",languageList[j]).c_str(),serverInterface->getSlot(i,true)->getIpAddress().c_str());
//										}
//										else {
//											snprintf(szMsg,8096,"The server has temporarily blocked IP Address [%s] from this game.",serverInterface->getSlot(i,true)->getIpAddress().c_str());
//										}
//
//										serverInterface->sendTextMessage(szMsg,-1, true,languageList[j]);
//									}
//									sleep(1);
//									serverInterface->getSlot(i,true)->close();
//								}
//							}
//						}
//					}
//					else if(listBoxFactions[i].mouseClick(x, y,advanceToItemStartingWith)) {
//						// Disallow CPU players to be observers
//						if(factionFiles[listBoxFactions[i].getSelectedItemIndex()] == formatString(GameConstants::OBSERVER_SLOTNAME) &&
//							(getSelectedPlayerControlTypeIndex(i) == ctCpuEasy || getSelectedPlayerControlTypeIndex(i) == ctCpu ||
//									getSelectedPlayerControlTypeIndex(i) == ctCpuUltra || getSelectedPlayerControlTypeIndex(i) == ctCpuMega)) {
//							listBoxFactions[i].setSelectedItemIndex(0);
//						}
//						//
//
//						MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
//						bool publishToServerEnabled =
//								cegui_manager.getCheckboxControlChecked(
//										cegui_manager.getControl("CheckboxPublishGame"));
//
//						if(publishToServerEnabled == true) {
//							needToRepublishToMasterserver = true;
//						}
//
//						if(hasNetworkGameSettings() == true)
//						{
//							needToSetChangedGameSettings = true;
//							lastSetChangedGameSettings   = time(NULL);
//						}
//					}
//					else if(listBoxTeams[i].mouseClick(x, y)) {

//						int selectedControlItemIndex = getSelectedPlayerFactionTypeIndex(i);
//						if(factionFiles[selectedControlItemIndex] != formatString(GameConstants::OBSERVER_SLOTNAME)) {
//							if(listBoxTeams[i].getSelectedItemIndex() + 1 != (GameConstants::maxPlayers + fpt_Observer)) {
//								lastSelectedTeamIndex[i] = listBoxTeams[i].getSelectedItemIndex();
//							}
//						}
//						else {
//							lastSelectedTeamIndex[i] = -1;
//						}
//
//						MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
//						bool publishToServerEnabled =
//								cegui_manager.getCheckboxControlChecked(
//										cegui_manager.getControl("CheckboxPublishGame"));
//
//						if(publishToServerEnabled == true) {
//							needToRepublishToMasterserver = true;
//						}
//
//						if(hasNetworkGameSettings() == true)
//						{
//							needToSetChangedGameSettings = true;
//							lastSetChangedGameSettings   = time(NULL);;
//						}
//					}
//					else if(labelPlayerNames[i].mouseClick(x, y)) {
//						ControlType ct= static_cast<ControlType>(getSelectedPlayerControlTypeIndex(i));
//						if(ct == ctHuman) {
//							setActiveInputLabel(&labelPlayerNames[i]);
//							break;
//						}
//					}
				}
			}
//        }

//		if(hasNetworkGameSettings() == true && listBoxPlayerStatus.mouseClick(x,y)) {
//			if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
//
//			soundRenderer.playFx(coreData.getClickSoundC());
//			if(getNetworkPlayerStatus()==npst_PickSettings)
//			{
//				listBoxPlayerStatus.setTextColor(Vec3f(1.0f,0.0f,0.0f));
//				listBoxPlayerStatus.setLighted(true);
//			}
//			else if(getNetworkPlayerStatus()==npst_BeRightBack)
//			{
//				listBoxPlayerStatus.setTextColor(Vec3f(1.0f,1.0f,0.0f));
//				listBoxPlayerStatus.setLighted(true);
//			}
//			else if(getNetworkPlayerStatus()==npst_Ready)
//			{
//				listBoxPlayerStatus.setTextColor(Vec3f(0.0f,1.0f,0.0f));
//				listBoxPlayerStatus.setLighted(false);
//			}
//
//			if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
//
//			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
//			bool publishToServerEnabled =
//					cegui_manager.getCheckboxControlChecked(
//							cegui_manager.getControl("CheckboxPublishGame"));
//
//            if(publishToServerEnabled == true) {
//                needToRepublishToMasterserver = true;
//            }
//
//            if(hasNetworkGameSettings() == true) {
//                needToSetChangedGameSettings = true;
//                lastSetChangedGameSettings   = time(NULL);
//            }
//		}
    }
	catch(const std::exception &ex) {
		char szBuf[8096]="";
		snprintf(szBuf,8096,"In [%s::%s %d]\nError detected:\n%s\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,ex.what());
		SystemFlags::OutputDebug(SystemFlags::debugError,szBuf);
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s",szBuf);

		showGeneralError=true;
		generalErrorToShow = szBuf;
	}

	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
}

void MenuStateCustomGame::updateAllResourceMultiplier() {
	for(int j=0; j<GameConstants::maxPlayers; ++j) {
		updateResourceMultiplier(j);
	}
}

void MenuStateCustomGame::updateResourceMultiplier(const int index) {
	//printf("Line: %d multiplier index: %d index: %d\n",__LINE__,listBoxRMultiplier[index].getSelectedItemIndex(),index);

	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	ControlType ct= static_cast<ControlType>(getSelectedPlayerControlTypeIndex(index));
	if(ct == ctCpuEasy || ct == ctNetworkCpuEasy) {
		cegui_manager.setSpinnerControlValue(cegui_manager.getControl(
				"SpinnerPlayer" + intToStr(index+1) + "Multiplier"),
				GameConstants::easyMultiplier);

//		printf("FILE: [%s: %d] value: %f\n",__FILE__,__LINE__,
//				cegui_manager.getSpinnerControlValue(cegui_manager.getControl(
//						"SpinnerPlayer" + intToStr(index+1) + "Multiplier")));

		cegui_manager.setControlEnabled(cegui_manager.getControl(
						"SpinnerPlayer" + intToStr(index+1) + "Multiplier"),
				getAllowNetworkScenario() == false);
		//listBoxRMultiplier[index].setSelectedItem(floatToStr(GameConstants::easyMultiplier,1));
		//listBoxRMultiplier[index].setEnabled(checkBoxScenario.getValue() == false);
	}
	else if(ct == ctCpu || ct == ctNetworkCpu) {
		cegui_manager.setSpinnerControlValue(cegui_manager.getControl(
				"SpinnerPlayer" + intToStr(index+1) + "Multiplier"),
				GameConstants::normalMultiplier);

//		printf("FILE: [%s: %d] value: %f\n",__FILE__,__LINE__,
//				cegui_manager.getSpinnerControlValue(cegui_manager.getControl(
//						"SpinnerPlayer" + intToStr(index+1) + "Multiplier")));

		cegui_manager.setControlEnabled(cegui_manager.getControl(
						"SpinnerPlayer" + intToStr(index+1) + "Multiplier"),
				getAllowNetworkScenario() == false);

		//listBoxRMultiplier[index].setSelectedItem(floatToStr(GameConstants::normalMultiplier,1));
		//listBoxRMultiplier[index].setEnabled(checkBoxScenario.getValue() == false);
	}
	else if(ct == ctCpuUltra || ct == ctNetworkCpuUltra) {
		cegui_manager.setSpinnerControlValue(cegui_manager.getControl(
				"SpinnerPlayer" + intToStr(index+1) + "Multiplier"),
				GameConstants::ultraMultiplier);

//		printf("FILE: [%s: %d] value: %f\n",__FILE__,__LINE__,
//				cegui_manager.getSpinnerControlValue(cegui_manager.getControl(
//						"SpinnerPlayer" + intToStr(index+1) + "Multiplier")));

		cegui_manager.setControlEnabled(cegui_manager.getControl(
						"SpinnerPlayer" + intToStr(index+1) + "Multiplier"),
				getAllowNetworkScenario() == false);

		//listBoxRMultiplier[index].setSelectedItem(floatToStr(GameConstants::ultraMultiplier,1));
		//listBoxRMultiplier[index].setEnabled(checkBoxScenario.getValue() == false);
	}
	else if(ct == ctCpuMega || ct == ctNetworkCpuMega) {
		cegui_manager.setSpinnerControlValue(cegui_manager.getControl(
				"SpinnerPlayer" + intToStr(index+1) + "Multiplier"),
				GameConstants::megaMultiplier);

//		printf("FILE: [%s: %d] value: %f\n",__FILE__,__LINE__,
//				cegui_manager.getSpinnerControlValue(cegui_manager.getControl(
//						"SpinnerPlayer" + intToStr(index+1) + "Multiplier")));

		cegui_manager.setControlEnabled(cegui_manager.getControl(
						"SpinnerPlayer" + intToStr(index+1) + "Multiplier"),
				getAllowNetworkScenario() == false);

		//listBoxRMultiplier[index].setSelectedItem(floatToStr(GameConstants::megaMultiplier,1));
		//listBoxRMultiplier[index].setEnabled(checkBoxScenario.getValue() == false);
	}
	//if(ct == ctHuman || ct == ctNetwork || ct == ctClosed) {
	else {

		cegui_manager.setSpinnerControlValue(cegui_manager.getControl(
				"SpinnerPlayer" + intToStr(index+1) + "Multiplier"),
				GameConstants::normalMultiplier);

//		printf("FILE: [%s: %d] value: %f\n",__FILE__,__LINE__,
//				cegui_manager.getSpinnerControlValue(cegui_manager.getControl(
//						"SpinnerPlayer" + intToStr(index+1) + "Multiplier")));

		cegui_manager.setControlEnabled(cegui_manager.getControl(
						"SpinnerPlayer" + intToStr(index+1) + "Multiplier"),
				getAllowNetworkScenario() == false);

		//listBoxRMultiplier[index].setSelectedItem(floatToStr(GameConstants::normalMultiplier,1));
		//listBoxRMultiplier[index].setEnabled(false);
		//!!!listBoxRMultiplier[index].setEnabled(checkBoxScenario.getValue() == false);
	}

	cegui_manager.setControlVisible(cegui_manager.getControl(
					"SpinnerPlayer" + intToStr(index+1) + "Multiplier"),
			ct != ctHuman && ct != ctNetwork && ct != ctClosed);
	//listBoxRMultiplier[index].setEditable(listBoxRMultiplier[index].getEnabled());
	//listBoxRMultiplier[index].setVisible(ct != ctHuman && ct != ctNetwork && ct != ctClosed);
	//listBoxRMultiplier[index].setVisible(ct != ctClosed);

	//printf("Line: %d multiplier index: %d index: %d\n",__LINE__,listBoxRMultiplier[index].getSelectedItemIndex(),index);
}

void MenuStateCustomGame::loadGameSettings(std::string fileName) {
	// Ensure we have set the gamesettings at least once
	GameSettings gameSettings = loadGameSettingsFromFile(fileName);
	if(gameSettings.getMap() == "") {
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

		loadGameSettings(&gameSettings);
	}

	ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
	serverInterface->setGameSettings(&gameSettings,false);

	MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
	MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));

	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	bool publishToServerEnabled =
			cegui_manager.getCheckboxControlChecked(
					cegui_manager.getControl("CheckboxPublishGame"));

	if(publishToServerEnabled == true) {
		needToRepublishToMasterserver = true;
	}

	if(hasNetworkGameSettings() == true) {
		needToSetChangedGameSettings = true;
		lastSetChangedGameSettings   = time(NULL);
	}
}

void MenuStateCustomGame::RestoreLastGameSettings() {
	loadGameSettings(SAVED_GAME_FILENAME);
}

bool MenuStateCustomGame::checkNetworkPlayerDataSynch(bool checkMapCRC,
		bool checkTileSetCRC, bool checkTechTreeCRC) {
	ServerInterface* serverInterface = NetworkManager::getInstance().getServerInterface();

	bool dataSynchCheckOk = true;
	for(int i= 0; i < mapInfo.players; ++i) {
		if(getSelectedPlayerControlTypeIndex(i) == ctNetwork) {

			MutexSafeWrapper safeMutex(serverInterface->getSlotMutex(i),CODE_AT_LINE);
			ConnectionSlot *slot = serverInterface->getSlot(i,false);
			if(	slot != NULL && slot->isConnected() &&
				(slot->getAllowDownloadDataSynch() == true ||
				 slot->getAllowGameDataSynchCheck() == true)) {

				if(checkMapCRC == true &&
						slot->getNetworkGameDataSynchCheckOkMap() == false) {
					dataSynchCheckOk = false;
					break;
				}
				if(checkTileSetCRC == true &&
						slot->getNetworkGameDataSynchCheckOkTile() == false) {
					dataSynchCheckOk = false;
					break;
				}
				if(checkTechTreeCRC == true &&
						slot->getNetworkGameDataSynchCheckOkTech() == false) {
					dataSynchCheckOk = false;
					break;
				}
			}
		}
	}

	return dataSynchCheckOk;
}

void MenuStateCustomGame::PlayNow(bool saveGame) {
	//printf("PlayNow triggered!\n");

	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();

	if(cegui_manager.getItemCountInComboBoxControl(cegui_manager.getControl("ComboBoxTechTree")) <= 0) {
		mainMessageBoxState=1;

		char szMsg[8096]="";
		strcpy(szMsg,"Cannot start game.\nThere are no tech-trees!\n");
		printf("%s",szMsg);

		showMessageBox(szMsg, "", false);
		return;
	}

	MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
	MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));

	if(saveGame == true) {
		saveGameSettingsToFile(SAVED_GAME_FILENAME);
	}

	forceWaitForShutdown = false;
	closeUnusedSlots();
	CoreData &coreData= CoreData::getInstance();
	SoundRenderer &soundRenderer= SoundRenderer::getInstance();
	soundRenderer.playFx(coreData.getClickSoundC());

	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	std::vector<string> randomFactionSelectionList;
	int RandomCount = 0;
	for(int i= 0; i < mapInfo.players; ++i) {
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

		// Check for random faction selection and choose the faction now
		if(getSelectedPlayerControlTypeIndex(i) != ctClosed) {

			if(//listBoxFactions[i].getSelectedItem() == formatString(GameConstants::RANDOMFACTION_SLOTNAME) &&
				//listBoxFactions[i].getItemCount() > 1) {
				getPlayerFactionTypeSelectedItem(i) == formatString(GameConstants::RANDOMFACTION_SLOTNAME) &&
				getSelectedPlayerFactionTypeItemCount(i) > 1) {

				if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d] i = %d\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,i);

				// Max 1000 tries to get a random, unused faction
				for(int findRandomFaction = 1; findRandomFaction < 1000; ++findRandomFaction) {
					Chrono seed(true);
					srand((unsigned int)seed.getCurTicks() + findRandomFaction);

					//int selectedFactionIndex = rand() % listBoxFactions[i].getItemCount();
					int selectedFactionIndex = rand() % getSelectedPlayerFactionTypeItemCount(i);
					//string selectedFactionName = listBoxFactions[i].getItem(selectedFactionIndex);
					string selectedFactionName = cegui_manager.getItemFromComboBoxControl(
							cegui_manager.getControl("ComboBoxPlayer" + intToStr(i+1) + "Faction"),selectedFactionIndex);

					if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d] selectedFactionName [%s] selectedFactionIndex = %d, findRandomFaction = %d\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,selectedFactionName.c_str(),selectedFactionIndex,findRandomFaction);

					if(	selectedFactionName != formatString(GameConstants::RANDOMFACTION_SLOTNAME) &&
						selectedFactionName != formatString(GameConstants::OBSERVER_SLOTNAME) &&
						std::find(randomFactionSelectionList.begin(),randomFactionSelectionList.end(),selectedFactionName) == randomFactionSelectionList.end()) {

						if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
						//listBoxFactions[i].setSelectedItem(selectedFactionName);
						setPlayerFactionTypeSelectedItem(i, selectedFactionName);

						randomFactionSelectionList.push_back(selectedFactionName);
						break;
					}
				}

				//if(listBoxFactions[i].getSelectedItem() == formatString(GameConstants::RANDOMFACTION_SLOTNAME)) {
				if(getPlayerFactionTypeSelectedItem(i) == formatString(GameConstants::RANDOMFACTION_SLOTNAME)) {
					if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d] RandomCount = %d\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,RandomCount);

					// Find first real faction and use it
					int factionIndexToUse = RandomCount;
					//for(int useIdx = 0; useIdx < listBoxFactions[i].getItemCount(); useIdx++) {
					for(int useIdx = 0; useIdx < getSelectedPlayerFactionTypeItemCount(i); useIdx++) {

						//string selectedFactionName = listBoxFactions[i].getItem(useIdx);
						string selectedFactionName = cegui_manager.getItemFromComboBoxControl(
								cegui_manager.getControl("ComboBoxPlayer" + intToStr(i+1) + "Faction"),useIdx);

						if(	selectedFactionName != formatString(GameConstants::RANDOMFACTION_SLOTNAME) &&
							selectedFactionName != formatString(GameConstants::OBSERVER_SLOTNAME)) {
							factionIndexToUse = useIdx;
							break;
						}
					}
					//listBoxFactions[i].setSelectedItemIndex(factionIndexToUse);
					setPlayerFactionTypeSelectedIndex(i,factionIndexToUse);

					//randomFactionSelectionList.push_back(listBoxFactions[i].getItem(factionIndexToUse));
					randomFactionSelectionList.push_back(
							cegui_manager.getItemFromComboBoxControl(
								cegui_manager.getControl("ComboBoxPlayer" + intToStr(i+1) + "Faction"),
									factionIndexToUse));
				}

				//if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d] i = %d, listBoxFactions[i].getSelectedItem() [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,i,listBoxFactions[i].getSelectedItem().c_str());

				RandomCount++;
			}
		}
	}

	if(RandomCount > 0) {
		needToSetChangedGameSettings = true;
	}

	safeMutex.ReleaseLock(true);
	safeMutexCLI.ReleaseLock(true);
	GameSettings gameSettings;
	loadGameSettings(&gameSettings, true);

	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
	ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();

	// Send the game settings to each client if we have at least one networked client
	safeMutex.Lock();
	safeMutexCLI.Lock();

	bool dataSynchCheckOk = checkNetworkPlayerDataSynch(true, true, true);

	// Ensure we have no dangling network players
	for(int i= 0; i < GameConstants::maxPlayers; ++i) {
		if(getSelectedPlayerControlTypeIndex(i) == ctNetworkUnassigned) {
			mainMessageBoxState=1;

			Lang &lang= Lang::getInstance();
			string sMsg = "";
			if(lang.hasString("NetworkSlotUnassignedErrorUI") == true) {
				sMsg = lang.getString("NetworkSlotUnassignedErrorUI");
			}
			else {
				sMsg = "Cannot start game.\nSome player(s) are not in a network game slot!";
			}

			showMessageBox(sMsg, "", false);

	    	const vector<string> languageList = serverInterface->getGameSettings()->getUniqueNetworkPlayerLanguages();
	    	for(unsigned int j = 0; j < languageList.size(); ++j) {
				char szMsg[8096]="";
				if(lang.hasString("NetworkSlotUnassignedError",languageList[j]) == true) {
					string msg_string = lang.getString("NetworkSlotUnassignedError");
#ifdef WIN32
					strncpy(szMsg,msg_string.c_str(),min((int)msg_string.length(),8095));
#else
					strncpy(szMsg,msg_string.c_str(),std::min((int)msg_string.length(),8095));
#endif
				}
				else {
					strcpy(szMsg,"Cannot start game, some player(s) are not in a network game slot!");
				}

				serverInterface->sendTextMessage(szMsg,-1, true,languageList[j]);
	    	}

			safeMutex.ReleaseLock();
			safeMutexCLI.ReleaseLock();
			return;
		}
	}

	if(dataSynchCheckOk == false) {
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
		mainMessageBoxState=1;
		showMessageBox("You cannot start the game because\none or more clients do not have the same game data!", "Data Mismatch Error", false);

		safeMutex.ReleaseLock();
		safeMutexCLI.ReleaseLock();
		return;
	}
	else {
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
		if( (hasNetworkGameSettings() == true &&
			 needToSetChangedGameSettings == true) || (RandomCount > 0)) {
			if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
			serverInterface->setGameSettings(&gameSettings,true);
			serverInterface->broadcastGameSetup(&gameSettings);

			needToSetChangedGameSettings    = false;
			lastSetChangedGameSettings      = time(NULL);
		}

		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

		// Last check, stop human player from being in same slot as network
		if(isMasterserverMode() == false) {
			bool hasHuman = false;
			for(int i= 0; i < mapInfo.players; ++i) {
				if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

				// Check for random faction selection and choose the faction now
				if(getSelectedPlayerControlTypeIndex(i) == ctHuman) {
					hasHuman = true;
					break;
				}
			}
			if(hasHuman == false) {
				mainMessageBoxState=1;

				Lang &lang= Lang::getInstance();
				string sMsg = lang.getString("NetworkSlotNoHumanErrorUI","",false,true);
				showMessageBox(sMsg, "", false);

		    	const vector<string> languageList = serverInterface->getGameSettings()->getUniqueNetworkPlayerLanguages();
		    	for(unsigned int j = 0; j < languageList.size(); ++j) {
					sMsg = lang.getString("NetworkSlotNoHumanError","",true);

					serverInterface->sendTextMessage(sMsg,-1, true,languageList[j]);
		    	}

				safeMutex.ReleaseLock();
				safeMutexCLI.ReleaseLock();
				return;
			}
		}

		// Tell the server Interface whether or not to publish game status updates to masterserver

		MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
		bool publishToServerEnabled =
				cegui_manager.getCheckboxControlChecked(
						cegui_manager.getControl("CheckboxPublishGame"));

		serverInterface->setNeedToRepublishToMasterserver(publishToServerEnabled);

		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
		bool bOkToStart = serverInterface->launchGame(&gameSettings);
		if(bOkToStart == true) {
			if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			bool publishToServerEditable =
					cegui_manager.getControlEnabled(
							cegui_manager.getControl("CheckboxPublishGame"));

			bool publishToServerEnabled =
					cegui_manager.getCheckboxControlChecked(
							cegui_manager.getControl("CheckboxPublishGame"));

			if( publishToServerEditable && publishToServerEnabled) {

				if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

				needToRepublishToMasterserver = true;
				lastMasterserverPublishing = 0;

				if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
			}
			needToBroadcastServerSettings = false;
			needToRepublishToMasterserver = false;
			lastNetworkPing               = time(NULL);
			safeMutex.ReleaseLock();
			safeMutexCLI.ReleaseLock();

			if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

			assert(program != NULL);

			if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

			cleanup();

			//MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			CEGUI::Window *ctl = cegui_manager.setCurrentLayout("CustomGameMenu.layout",containerName);
			cegui_manager.setControlVisible(ctl,false);

			Game *newGame = new Game(program, &gameSettings, this->headlessServerMode);
			forceWaitForShutdown = false;
			program->setState(newGame);
			return;
		}
		else {
			safeMutex.ReleaseLock();
			safeMutexCLI.ReleaseLock();
		}
	}
}

void MenuStateCustomGame::mouseMove(int x, int y, const MouseState *ms) {
	if(isMasterserverMode() == true) {
		return;
	}

	//if (mainMessageBox.getEnabled()) {
	//	mainMessageBox.mouseMove(x, y);
	//}
	//buttonReturn.mouseMove(x, y);
	//buttonPlayNow.mouseMove(x, y);
	//buttonRestoreLastSettings.mouseMove(x, y);
	//buttonClearBlockedPlayers.mouseMove(x, y);

	//for(int i = 0; i < GameConstants::maxPlayers; ++i) {
		//listBoxRMultiplier[i].mouseMove(x, y);
        //listBoxControls[i].mouseMove(x, y);
        //buttonBlockPlayers[i].mouseMove(x, y);
        //listBoxFactions[i].mouseMove(x, y);
		//listBoxTeams[i].mouseMove(x, y);
    //}

	//listBoxMap.mouseMove(x, y);

	//if(checkBoxAdvanced.getValue() == 1) {
		//listBoxFogOfWar.mouseMove(x, y);
		//checkBoxAllowObservers.mouseMove(x, y);

		//checkBoxEnableSwitchTeamMode.mouseMove(x, y);
		//listBoxAISwitchTeamAcceptPercent.mouseMove(x, y);
		//listBoxFallbackCpuMultiplier.mouseMove(x, y);

		//labelNetworkPauseGameForLaggedClients.mouseMove(x, y);
		//checkBoxNetworkPauseGameForLaggedClients.mouseMove(x, y);

		//labelAllowTeamUnitSharing.mouseMove(x,y);
		//checkBoxAllowTeamUnitSharing.mouseMove(x,y);
		//labelAllowTeamResourceSharing.mouseMove(x,y);
		//checkBoxAllowTeamResourceSharing.mouseMove(x,y);
	//}
	//checkBoxAllowInGameJoinPlayer.mouseMove(x, y);

	//checkBoxAllowNativeLanguageTechtree.mouseMove(x, y);

	//listBoxTileset.mouseMove(x, y);
	//listBoxMapFilter.mouseMove(x, y);
	//listBoxTechTree.mouseMove(x, y);
	//checkBoxPublishServer.mouseMove(x, y);

	//checkBoxAdvanced.mouseMove(x, y);

	//checkBoxScenario.mouseMove(x, y);
	//listBoxScenario.mouseMove(x, y);
}

bool MenuStateCustomGame::isMasterserverMode() const {
	return (this->headlessServerMode == true && this->masterserverModeMinimalResources == true);
	//return false;
}

bool MenuStateCustomGame::isVideoPlaying() {
	bool result = false;
	if(factionVideo != NULL) {
		result = factionVideo->isPlaying();
	}
	return result;
}

void MenuStateCustomGame::render() {
	try {
		Renderer &renderer= Renderer::getInstance();

		//if(mainMessageBox.getEnabled() == false) {
//			if(factionTexture != NULL) {
//				if(factionVideo == NULL || factionVideo->isPlaying() == false) {
//					renderer.renderTextureQuad(800,600,200,150,factionTexture,1.0f);
//				}
//			}
		//}
		if(factionVideo != NULL) {
			if(factionVideo->isPlaying() == true) {
				factionVideo->playFrame(false);
			}
			else {
				if(GlobalStaticFlags::getIsNonGraphicalModeEnabled() == false &&
						::Shared::Graphics::VideoPlayer::hasBackEndVideoPlayer() == true) {
					if(factionVideo != NULL) {
						factionVideo->closePlayer();
						delete factionVideo;
						factionVideo = NULL;

						ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
						if(serverInterface != NULL) {
							initFactionPreview(serverInterface->getGameSettings());
						}
					}
				}
			}
		}

		//if(mainMessageBox.getEnabled()) {
		//	renderer.renderMessageBox(&mainMessageBox);

			//renderer.renderButton(&buttonReturn);
		//}
		//else {
//			if(mapPreviewTexture != NULL) {
//				//renderer.renderTextureQuad(5,185,150,150,mapPreviewTexture,1.0f);
//				renderer.renderTextureQuad(	this->render_mapPreviewTexture_X,
//											this->render_mapPreviewTexture_Y,
//											this->render_mapPreviewTexture_W,
//											this->render_mapPreviewTexture_H,
//											mapPreviewTexture,1.0f);
//				if(this->zoomedMap==true) {
//					return;
//				}
//				//printf("=================> Rendering map preview texture\n");
//			}
			if(scenarioLogoTexture != NULL) {
				renderer.renderTextureQuad(300,350,400,300,scenarioLogoTexture,1.0f);
				//renderer.renderBackground(scenarioLogoTexture);
			}

			//renderer.renderButton(&buttonReturn);
			//renderer.renderButton(&buttonPlayNow);
			//renderer.renderButton(&buttonRestoreLastSettings);

			// Get a reference to the player texture cache
			//std::map<int,Texture2D *> &crcPlayerTextureCache = CacheManager::getCachedItem< std::map<int,Texture2D *> >(GameConstants::playerTextureCacheLookupKey);

			// START - this code ensure player title and player names don't overlap
			//int offsetPosition=0;
//		    for(int i=0; i < GameConstants::maxPlayers; ++i) {
//				FontMetrics *fontMetrics= NULL;
//				if(Renderer::renderText3DEnabled == false) {
//					fontMetrics = labelPlayers[i].getFont()->getMetrics();
//				}
//				else {
//					fontMetrics = labelPlayers[i].getFont3D()->getMetrics();
//				}
//				if(fontMetrics == NULL) {
//					throw megaglest_runtime_error("fontMetrics == NULL");
//				}
//				int curWidth = (fontMetrics->getTextWidth(labelPlayers[i].getText()));
//				int newOffsetPosition = labelPlayers[i].getX() + curWidth + 2;
//
//				//printf("labelPlayers[i].getX() = %d curWidth = %d labelPlayerNames[i].getX() = %d offsetPosition = %d newOffsetPosition = %d [%s]\n",labelPlayers[i].getX(),curWidth,labelPlayerNames[i].getX(),offsetPosition,newOffsetPosition,labelPlayers[i].getText().c_str());
//
//				if(labelPlayers[i].getX() + curWidth >= labelPlayerNames[i].getX()) {
//					if(offsetPosition < newOffsetPosition) {
//						offsetPosition = newOffsetPosition;
//					}
//				}
//		    }
		    // END

			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
//    		cegui_manager.setControlVisible(
//    			cegui_manager.getControl("ButtonClearBlockedPlayers"),false);

		    ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
		    if(hasNetworkGameSettings() == true) {
		    	//renderer.renderListBox(&listBoxPlayerStatus);
		    	if( serverInterface != NULL &&
		    		serverInterface->getServerSocket() != NULL ) {
		    		//renderer.renderButton(&buttonClearBlockedPlayers);

		    		//cegui_manager.setControlVisible(
		    		//	cegui_manager.getControl("ButtonClearBlockedPlayers"),true);
		    	}
		    }

			for(int i = 0; i < GameConstants::maxPlayers; ++i) {
		    	if(getSelectedPlayerControlTypeIndex(i) == ctNetworkUnassigned) {
		    		//printf("Player #%d [%s] control = %d\n",i,labelPlayerNames[i].getText().c_str(),listBoxControls[i].getSelectedItemIndex());

					//labelPlayers[i].setVisible(true);
		    		setPlayerNumberVisible(i, true);

					//labelPlayerNames[i].setVisible(true);
		    		setPlayerNameVisible(i,true);
					//listBoxControls[i].setVisible(true);
					setPlayerControlTypeVisible(i,true);
					//listBoxFactions[i].setVisible(true);
					cegui_manager.setControlVisible(
							cegui_manager.getControl(
									"ComboBoxPlayer" + intToStr(i+1) + "Faction"),
									true);
					setPlayerTeamVisible(i,true);
					//labelNetStatus[i].setVisible(true);
					setPlayerVersionVisible(i,true);
		    	}

				if( hasNetworkGameSettings() == true &&
						getSelectedPlayerControlTypeIndex(i) != ctClosed) {

					//renderer.renderLabel(&labelPlayerStatus[i]);
				}

//				if(crcPlayerTextureCache[i] != NULL) {
//					// Render the player # label the player's color
//					Vec3f playerColor = crcPlayerTextureCache[i]->getPixmap()->getPixel3f(0, 0);
//					renderer.renderLabel(&labelPlayers[i],&playerColor);
//				}
//				else {
//					renderer.renderLabel(&labelPlayers[i]);
//				}

//				if(offsetPosition > 0) {
//					labelPlayerNames[i].setX(offsetPosition);
//				}
//				renderer.renderLabel(&labelPlayerNames[i]);

				//renderer.renderListBox(&listBoxControls[i]);

				cegui_manager.setControlVisible("ButtonPlayer" + intToStr(i+1) + "Block",false);
				if( hasNetworkGameSettings() == true &&
						getSelectedPlayerControlTypeIndex(i) != ctClosed) {

					//renderer.renderLabel(&labelPlayerStatus[i]);

					if(getSelectedPlayerControlTypeIndex(i) == ctNetwork ||
							getSelectedPlayerControlTypeIndex(i) == ctNetworkUnassigned) {

						ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
						if( serverInterface != NULL &&
							serverInterface->getSlot(i,true) != NULL &&
		                    serverInterface->getSlot(i,true)->isConnected()) {
							//renderer.renderButton(&buttonBlockPlayers[i]);
							cegui_manager.setControlVisible("ButtonPlayer" + intToStr(i+1) + "Block",true);
							cegui_manager.setControlOnTop("ButtonPlayer" + intToStr(i+1) + "Block",true);
						}
					}
				}

				//if(getSelectedPlayerControlTypeIndex(i) != ctClosed) {
					//renderer.renderListBox(&listBoxRMultiplier[i]);

					//renderer.renderListBox(&listBoxFactions[i]);
					//renderer.renderListBox(&listBoxTeams[i]);
					//renderer.renderLabel(&labelNetStatus[i]);
				//}
			}

			//renderer.renderLabel(&labelLocalGameVersion);
			//renderer.renderLabel(&labelLocalIP);
			//renderer.renderLabel(&labelMap);

			//if(checkBoxAdvanced.getValue() == 1) {
				//renderer.renderLabel(&labelFogOfWar);
				//renderer.renderLabel(&labelAllowObservers);
				//renderer.renderLabel(&labelFallbackCpuMultiplier);

				//renderer.renderLabel(&labelEnableSwitchTeamMode);
				//renderer.renderLabel(&labelAISwitchTeamAcceptPercent);

				//renderer.renderListBox(&listBoxFogOfWar);
				//renderer.renderCheckBox(&checkBoxAllowObservers);

				//renderer.renderCheckBox(&checkBoxEnableSwitchTeamMode);
				//renderer.renderListBox(&listBoxAISwitchTeamAcceptPercent);
				//renderer.renderListBox(&listBoxFallbackCpuMultiplier);

				//renderer.renderLabel(&labelAllowTeamUnitSharing);
				//renderer.renderCheckBox(&checkBoxAllowTeamUnitSharing);

				//renderer.renderLabel(&labelAllowTeamResourceSharing);
				//renderer.renderCheckBox(&checkBoxAllowTeamResourceSharing);
			//}
			//renderer.renderLabel(&labelAllowInGameJoinPlayer);
			//renderer.renderCheckBox(&checkBoxAllowInGameJoinPlayer);

			//renderer.renderLabel(&labelTileset);
			//renderer.renderLabel(&labelMapFilter);
			//renderer.renderLabel(&labelTechTree);
			//renderer.renderLabel(&labelControl);
			//renderer.renderLabel(&labelFaction);
			//renderer.renderLabel(&labelTeam);
			//renderer.renderLabel(&labelMapInfo);
			//renderer.renderLabel(&labelAdvanced);

			//renderer.renderListBox(&listBoxMap);
			//renderer.renderListBox(&listBoxTileset);
			//renderer.renderListBox(&listBoxMapFilter);
			//renderer.renderListBox(&listBoxTechTree);
			//renderer.renderCheckBox(&checkBoxAdvanced);


//			if(checkBoxPublishServer.getEditable())
//			{
//				renderer.renderCheckBox(&checkBoxPublishServer);
//				renderer.renderLabel(&labelPublishServer);
//				//renderer.renderLabel(&labelGameName);
//				if(checkBoxAdvanced.getValue() == 1) {
//					renderer.renderLabel(&labelNetworkPauseGameForLaggedClients);
//					renderer.renderCheckBox(&checkBoxNetworkPauseGameForLaggedClients);
//				}
//			}

			//renderer.renderCheckBox(&checkBoxScenario);
			//renderer.renderLabel(&labelScenario);
			//if(checkBoxScenario.getValue() == true) {
				//renderer.renderListBox(&listBoxScenario);
			//}

			//renderer.renderLabel(&labelAllowNativeLanguageTechtree);
			//renderer.renderCheckBox(&checkBoxAllowNativeLanguageTechtree);
		//}

		if(program != NULL) program->renderProgramMsgBox();

		if( enableMapPreview == true &&
			mapPreview.hasFileLoaded() == true) {

		    if(mapPreviewTexture == NULL) {

				MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
				int selectedFogOfWarIndex = cegui_manager.getSelectedItemIndexFromComboBoxControl(cegui_manager.getControl("ComboBoxFogOfWar"));

		    	bool renderAll = (selectedFogOfWarIndex == 2);
		    	//printf("=================> Rendering map preview into a texture BEFORE (%p)\n", mapPreviewTexture);

		    	//renderer.renderMapPreview(&mapPreview, renderAll, 10, 350,&mapPreviewTexture);
		    	renderer.renderMapPreview(&mapPreview, renderAll,
		    			this->render_mapPreviewTexture_X,
		    			this->render_mapPreviewTexture_Y,
		    			&mapPreviewTexture);

		    	//printf("Load Map [%s] [%s]\n",mapPreview.getMapFileLoaded().c_str(),mapPreviewTexture->getPath().c_str());
		    	cegui_manager.setImageForControl("TextureMapPreview",mapPreviewTexture, "ImageMapPreview",true);

		    	//printf("=================> Rendering map preview into a texture AFTER (%p)\n", mapPreviewTexture);
		    }
		}

		//if(mainMessageBox.getEnabled() == false) {
			if(hasNetworkGameSettings() == true) {
				renderer.renderChatManager(&chatManager);
			}
		//}
		renderer.renderConsole(&console,showFullConsole,true);
	}
	catch(const std::exception &ex) {
		char szBuf[8096]="";
		snprintf(szBuf,8096,"In [%s::%s %d]\nError detected:\n%s\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,ex.what());
		//throw megaglest_runtime_error(szBuf);

		SystemFlags::OutputDebug(SystemFlags::debugError,szBuf);
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s",szBuf);
		showGeneralError=true;
		generalErrorToShow = szBuf;
	}
}

void MenuStateCustomGame::switchSetupForSlots(SwitchSetupRequest **switchSetupRequests,
		ServerInterface *& serverInterface, int startIndex, int endIndex, bool onlyNetworkUnassigned) {
    for(int i= startIndex; i < endIndex; ++i) {
		if(switchSetupRequests[i] != NULL) {
			//printf("Switch slot = %d control = %d newIndex = %d currentindex = %d onlyNetworkUnassigned = %d\n",i,listBoxControls[i].getSelectedItemIndex(),switchSetupRequests[i]->getToFactionIndex(),switchSetupRequests[i]->getCurrentFactionIndex(),onlyNetworkUnassigned);

			if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d] switchSetupRequests[i]->getSwitchFlags() = %d\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,switchSetupRequests[i]->getSwitchFlags());

			if(onlyNetworkUnassigned == true && getSelectedPlayerControlTypeIndex(i) != ctNetworkUnassigned) {
				if(i < mapInfo.players || (i >= mapInfo.players && getSelectedPlayerControlTypeIndex(i) != ctNetwork)) {
					continue;
				}
			}

			if(getSelectedPlayerControlTypeIndex(i) == ctNetwork ||
					getSelectedPlayerControlTypeIndex(i) == ctNetworkUnassigned) {

				if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d] switchSetupRequests[i]->getToFactionIndex() = %d\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,switchSetupRequests[i]->getToSlotIndex());

				if(switchSetupRequests[i]->getToSlotIndex() != -1) {
					int newFactionIdx = switchSetupRequests[i]->getToSlotIndex();

					//printf("switchSlot request from %d to %d\n",switchSetupRequests[i]->getCurrentFactionIndex(),switchSetupRequests[i]->getToFactionIndex());
					int switchFactionIdx = switchSetupRequests[i]->getCurrentSlotIndex();
					if(serverInterface->switchSlot(switchFactionIdx,newFactionIdx)) {
						try {
							MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();

							ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
							ConnectionSlot *slot = serverInterface->getSlot(newFactionIdx,true);

							if(switchSetupRequests[i]->getSelectedFactionName() != "" &&
								(slot != NULL &&
								 switchSetupRequests[i]->getSelectedFactionName() != Lang::getInstance().getString("DataMissing",slot->getNetworkPlayerLanguage(),true) &&
								 switchSetupRequests[i]->getSelectedFactionName() != "???DataMissing???")) {
								//listBoxFactions[newFactionIdx].setSelectedItem(switchSetupRequests[i]->getSelectedFactionName());

								//!!!
								setPlayerFactionTypeSelectedItem(newFactionIdx, switchSetupRequests[i]->getSelectedFactionName());
							}
							if(switchSetupRequests[i]->getToTeam() != -1) {
								setSelectedPlayerTeamIndex(newFactionIdx,switchSetupRequests[i]->getToTeam());
							}
							if(switchSetupRequests[i]->getNetworkPlayerName() != "") {
								if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d] i = %d, labelPlayerNames[newFactionIdx].getText() [%s] switchSetupRequests[i]->getNetworkPlayerName() [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,i,getPlayerNameText(newFactionIdx).c_str(),switchSetupRequests[i]->getNetworkPlayerName().c_str());
								setPlayerNameText(newFactionIdx,switchSetupRequests[i]->getNetworkPlayerName());
							}

							if(getSelectedPlayerControlTypeIndex(switchFactionIdx) == ctNetworkUnassigned) {

								serverInterface->removeSlot(switchFactionIdx);
								//listBoxControls[switchFactionIdx].setSelectedItemIndex(ctClosed);
								setPlayerControlTypeSelectedIndex(switchFactionIdx,ctClosed);

								//labelPlayers[switchFactionIdx].setVisible(switchFactionIdx+1 <= mapInfo.players);
								setPlayerNumberVisible(switchFactionIdx, switchFactionIdx+1 <= mapInfo.players);

								setPlayerNameVisible(switchFactionIdx,switchFactionIdx+1 <= mapInfo.players);
						        //listBoxControls[switchFactionIdx].setVisible(switchFactionIdx+1 <= mapInfo.players);
								setPlayerControlTypeVisible(switchFactionIdx,switchFactionIdx+1 <= mapInfo.players);
						        //listBoxFactions[switchFactionIdx].setVisible(switchFactionIdx+1 <= mapInfo.players);
								cegui_manager.setControlVisible(
									cegui_manager.getControl(
										"ComboBoxPlayer" + intToStr(switchFactionIdx+1) + "Faction"),
										switchFactionIdx+1 <= mapInfo.players);
								setPlayerTeamVisible(switchFactionIdx,switchFactionIdx+1 <= mapInfo.players);
								//labelNetStatus[switchFactionIdx].setVisible(switchFactionIdx+1 <= mapInfo.players);
								setPlayerVersionVisible(switchFactionIdx,switchFactionIdx+1 <= mapInfo.players);
							}
						}
						catch(const runtime_error &e) {
							SystemFlags::OutputDebug(SystemFlags::debugError,"In [%s::%s Line: %d] Error [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,e.what());
							if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d] caught exception error = [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,e.what());
						}
					}
				}
				else {
					try {
						int factionIdx = switchSetupRequests[i]->getCurrentSlotIndex();
						ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
						ConnectionSlot *slot = serverInterface->getSlot(factionIdx,true);

						if(switchSetupRequests[i]->getSelectedFactionName() != "" &&
							(slot != NULL &&
							 switchSetupRequests[i]->getSelectedFactionName() != Lang::getInstance().getString("DataMissing",slot->getNetworkPlayerLanguage(),true) &&
							 switchSetupRequests[i]->getSelectedFactionName() != "???DataMissing???")) {

							//listBoxFactions[i].setSelectedItem(switchSetupRequests[i]->getSelectedFactionName());
							setPlayerFactionTypeSelectedItem(i,switchSetupRequests[i]->getSelectedFactionName());
						}
						if(switchSetupRequests[i]->getToTeam() != -1) {
							setSelectedPlayerTeamIndex(i,switchSetupRequests[i]->getToTeam());
						}

						if((switchSetupRequests[i]->getSwitchFlags() & ssrft_NetworkPlayerName) == ssrft_NetworkPlayerName) {
							if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d] i = %d, switchSetupRequests[i]->getSwitchFlags() = %d, switchSetupRequests[i]->getNetworkPlayerName() [%s], labelPlayerNames[i].getText() [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,i,switchSetupRequests[i]->getSwitchFlags(),switchSetupRequests[i]->getNetworkPlayerName().c_str(),getPlayerNameText(i).c_str());

							if(switchSetupRequests[i]->getNetworkPlayerName() != GameConstants::NETWORK_SLOT_UNCONNECTED_SLOTNAME) {
								setPlayerNameText(i,switchSetupRequests[i]->getNetworkPlayerName());
							}
							else {
								setPlayerNameText(i,"");
							}
						}
					}
					catch(const runtime_error &e) {
						SystemFlags::OutputDebug(SystemFlags::debugError,"In [%s::%s Line: %d] Error [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,e.what());
						if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d] caught exception error = [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,e.what());
					}
				}
			}

			delete switchSetupRequests[i];
			switchSetupRequests[i]=NULL;
		}
	}
}

void MenuStateCustomGame::update() {
	Chrono chrono;
	if(SystemFlags::getSystemSettingType(SystemFlags::debugPerformance).enabled) chrono.start();

	// Test openal buffer underrun issue
	//sleep(200);
	// END

	MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
	MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));

	try {
		if(serverInitError == true) {
			if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

			if(showGeneralError) {
				if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);


				showGeneralError=false;
				mainMessageBoxState=1;
				showMessageBox( generalErrorToShow, "Error", false);
			}

			if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

			if(this->headlessServerMode == false) {
				return;
			}
		}
		ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
		Lang& lang= Lang::getInstance();

		MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
		if( serverInterface != NULL && serverInterface->getServerSocket() != NULL ) {
			//buttonClearBlockedPlayers.setEditable( serverInterface->getServerSocket()->hasBlockedIPAddresses());

			bool canClear = serverInterface->getServerSocket()->hasBlockedIPAddresses();

    		//cegui_manager.setControlEnabled(
    			//cegui_manager.getControl("ButtonClearBlockedPlayers"),
    			//	serverInterface->getServerSocket()->hasBlockedIPAddresses());
		    cegui_manager.setControlVisible(
		    	cegui_manager.getControl("ButtonClearBlockedPlayers"),canClear);
		    if(canClear) {
		    	cegui_manager.setControlOnTop(cegui_manager.getControl("ButtonClearBlockedPlayers"),true);
		    }
		}
		else {
			cegui_manager.setControlVisible(
					    	cegui_manager.getControl("ButtonClearBlockedPlayers"),false);
		}

		if(this->autoloadScenarioName != "") {
			setSelectedNetworkScenario(formatString(this->autoloadScenarioName));

			if(getNetworkScenarioSelectedItem() != formatString(this->autoloadScenarioName)) {
				mainMessageBoxState=1;
				showMessageBox( "Could not find scenario name: " + formatString(this->autoloadScenarioName), "Scenario Missing", false);
				this->autoloadScenarioName = "";
			}
			else {
				loadScenarioInfo(Scenario::getScenarioPath(dirList, scenarioFiles.at(getSelectedNetworkScenarioIndex())), &scenarioInfo);
				//labelInfo.setText(scenarioInfo.desc);

				SoundRenderer &soundRenderer= SoundRenderer::getInstance();
				CoreData &coreData= CoreData::getInstance();
				soundRenderer.playFx(coreData.getClickSoundC());
				//launchGame();
				PlayNow(true);
				return;
			}
		}

		if(needToLoadTextures) {
			// this delay is done to make it possible to switch faster
			if(difftime((long int)time(NULL), previewLoadDelayTimer) >= 2){
				//loadScenarioPreviewTexture();
				needToLoadTextures= false;
			}
		}

		//bool haveAtLeastOneNetworkClientConnected = false;
		bool hasOneNetworkSlotOpen = false;
		int currentConnectionCount=0;
		Config &config = Config::getInstance();

		bool masterServerErr = showMasterserverError;

		if(SystemFlags::getSystemSettingType(SystemFlags::debugPerformance).enabled && chrono.getMillis() > 0) SystemFlags::OutputDebug(SystemFlags::debugPerformance,"In [%s::%s Line: %d] took msecs: %lld\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,chrono.getMillis());
		if(SystemFlags::getSystemSettingType(SystemFlags::debugPerformance).enabled && chrono.getMillis() > 0) chrono.start();

		if(masterServerErr) {
			if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

			if(EndsWith(masterServererErrorToShow, "wrong router setup") == true) {
				masterServererErrorToShow=lang.getString("WrongRouterSetup");
			}

			Lang &lang= Lang::getInstance();
			string publishText = " (disabling publish)";
			if(lang.hasString("PublishDisabled") == true) {
				publishText = lang.getString("PublishDisabled");
			}

            masterServererErrorToShow += publishText;
			showMasterserverError=false;
			mainMessageBoxState=1;
			showMessageBox( masterServererErrorToShow, lang.getString("ErrorFromMasterserver","",false,true), false);

			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();

			if(this->headlessServerMode == false) {
				//checkBoxPublishServer.setValue(false);
				cegui_manager.setCheckboxControlChecked(cegui_manager.getControl("CheckboxPublishGame"),false);
			}

			bool publishToServerEnabled =
					cegui_manager.getCheckboxControlChecked(
							cegui_manager.getControl("CheckboxPublishGame"));

            ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
            serverInterface->setPublishEnabled(publishToServerEnabled);
		}
		else if(showGeneralError) {
			if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

			showGeneralError=false;
			mainMessageBoxState=1;
			showMessageBox( generalErrorToShow, "Error", false);
		}

		if(SystemFlags::getSystemSettingType(SystemFlags::debugPerformance).enabled && chrono.getMillis() > 0) SystemFlags::OutputDebug(SystemFlags::debugPerformance,"In [%s::%s Line: %d] took msecs: %lld\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,chrono.getMillis());
		if(SystemFlags::getSystemSettingType(SystemFlags::debugPerformance).enabled && chrono.getMillis() > 0) chrono.start();

		if(this->headlessServerMode == true && serverInterface == NULL) {
			throw megaglest_runtime_error("serverInterface == NULL");
		}
		if(this->headlessServerMode == true && serverInterface->getGameSettingsUpdateCount() > lastMasterServerSettingsUpdateCount &&
				serverInterface->getGameSettings() != NULL) {
			const GameSettings *settings = serverInterface->getGameSettings();
			//printf("\n\n\n\n=====#1 got settings [%d] [%d]:\n%s\n",lastMasterServerSettingsUpdateCount,serverInterface->getGameSettingsUpdateCount(),settings->toString().c_str());

			lastMasterServerSettingsUpdateCount = serverInterface->getGameSettingsUpdateCount();
			//printf("#2 custom menu got map [%s]\n",settings->getMap().c_str());

			setupUIFromGameSettings(*settings);

            GameSettings gameSettings;
            loadGameSettings(&gameSettings);

            //printf("\n\n\n\n=====#1.1 got settings [%d] [%d]:\n%s\n",lastMasterServerSettingsUpdateCount,serverInterface->getGameSettingsUpdateCount(),gameSettings.toString().c_str());

		}
		if(this->headlessServerMode == true && serverInterface->getMasterserverAdminRequestLaunch() == true) {
			serverInterface->setMasterserverAdminRequestLaunch(false);
			safeMutex.ReleaseLock();
			safeMutexCLI.ReleaseLock();

			PlayNow(false);
			return;
		}

		// handle setting changes from clients
		SwitchSetupRequest ** switchSetupRequests = serverInterface->getSwitchSetupRequests();
		//!!!
		switchSetupForSlots(switchSetupRequests, serverInterface, 0, mapInfo.players, false);
		switchSetupForSlots(switchSetupRequests, serverInterface, mapInfo.players, GameConstants::maxPlayers, true);

		if(SystemFlags::getSystemSettingType(SystemFlags::debugPerformance).enabled && chrono.getMillis() > 0) SystemFlags::OutputDebug(SystemFlags::debugPerformance,"In [%s::%s Line: %d] took msecs: %lld\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,chrono.getMillis());
		if(SystemFlags::getSystemSettingType(SystemFlags::debugPerformance).enabled && chrono.getMillis() > 0) chrono.start();

		GameSettings gameSettings;
		loadGameSettings(&gameSettings);

		//listBoxAISwitchTeamAcceptPercent.setEnabled(checkBoxEnableSwitchTeamMode.getValue());
		//MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
		CEGUI::Window *ctl = cegui_manager.getControl(
						"ComboAIAcceptPercent");
		cegui_manager.setControlEnabled(ctl,getAllowSwitchTeams());

		for(int index = 0; index < GameConstants::maxPlayers; ++index) {
			setPlayerStatusImageVisible(index, false);
		}

		int factionCount = 0;
		for(int i= 0; i< mapInfo.players; ++i) {
			if(hasNetworkGameSettings() == true) {
				if(getSelectedPlayerControlTypeIndex(i) != ctClosed) {
					int slotIndex = factionCount;
					if(getSelectedPlayerControlTypeIndex(i) == ctHuman) {
						switch(gameSettings.getNetworkPlayerStatuses(slotIndex)) {
							case npst_BeRightBack:
								//labelPlayerStatus[i].setTexture(CoreData::getInstance().getStatusBRBTexture());
								setPlayerStatusImage(i, CoreData::getInstance().getStatusBRBTexture());
								setPlayerStatusImageVisible(i, true);

								break;
							case npst_Ready:
								//labelPlayerStatus[i].setTexture(CoreData::getInstance().getStatusReadyTexture());
								setPlayerStatusImage(i, CoreData::getInstance().getStatusReadyTexture());
								setPlayerStatusImageVisible(i, true);

								break;
							case npst_PickSettings:
								//labelPlayerStatus[i].setTexture(CoreData::getInstance().getStatusNotReadyTexture());
								setPlayerStatusImage(i, CoreData::getInstance().getStatusNotReadyTexture());
								setPlayerStatusImageVisible(i, true);

								break;
							case npst_Disconnected:
								//labelPlayerStatus[i].setTexture(NULL);
								setPlayerStatusImage(i, NULL);
								break;

							default:
								//labelPlayerStatus[i].setTexture(NULL);
								setPlayerStatusImage(i, NULL);
								break;
						}
					}
					else {
						//labelPlayerStatus[i].setTexture(NULL);
						setPlayerStatusImage(i, NULL);
					}

					factionCount++;
				}
				else {
					//labelPlayerStatus[i].setTexture(NULL);
					setPlayerStatusImage(i, NULL);
				}
			}

			if(getSelectedPlayerControlTypeIndex(i) == ctNetwork ||
					getSelectedPlayerControlTypeIndex(i) == ctNetworkUnassigned) {
				hasOneNetworkSlotOpen=true;

				if(serverInterface->getSlot(i,true) != NULL &&
                   serverInterface->getSlot(i,true)->isConnected()) {

					if(hasNetworkGameSettings() == true) {
						switch(serverInterface->getSlot(i,true)->getNetworkPlayerStatus()) {
							case npst_BeRightBack:
								//labelPlayerStatus[i].setTexture(CoreData::getInstance().getStatusBRBTexture());
								setPlayerStatusImage(i, CoreData::getInstance().getStatusBRBTexture());
								setPlayerStatusImageVisible(i, true);

								break;
							case npst_Ready:
								//labelPlayerStatus[i].setTexture(CoreData::getInstance().getStatusReadyTexture());
								setPlayerStatusImage(i, CoreData::getInstance().getStatusReadyTexture());
								setPlayerStatusImageVisible(i, true);

								break;
							case npst_PickSettings:
							default:
								//labelPlayerStatus[i].setTexture(CoreData::getInstance().getStatusNotReadyTexture());
								setPlayerStatusImage(i, CoreData::getInstance().getStatusNotReadyTexture());
								setPlayerStatusImageVisible(i, true);

								break;
						}
					}

					serverInterface->getSlot(i,true)->setName(getPlayerNameText(i));

					//printf("FYI we have at least 1 client connected, slot = %d'\n",i);

					//haveAtLeastOneNetworkClientConnected = true;
					if(serverInterface->getSlot(i,true) != NULL &&
                       serverInterface->getSlot(i,true)->getConnectHasHandshaked()) {
						currentConnectionCount++;
                    }
					string label = (serverInterface->getSlot(i,true) != NULL ? serverInterface->getSlot(i,true)->getVersionString() : "");

					if(serverInterface->getSlot(i,true) != NULL &&
					   serverInterface->getSlot(i,true)->getAllowDownloadDataSynch() == true &&
					   serverInterface->getSlot(i,true)->getAllowGameDataSynchCheck() == true) {
						if(serverInterface->getSlot(i,true)->getNetworkGameDataSynchCheckOk() == false) {
							label += " -waiting to synch:";
							if(serverInterface->getSlot(i,true)->getNetworkGameDataSynchCheckOkMap() == false) {
								label = label + " map";
							}
							if(serverInterface->getSlot(i,true)->getNetworkGameDataSynchCheckOkTile() == false) {
								label = label + " tile";
							}
							if(serverInterface->getSlot(i,true)->getNetworkGameDataSynchCheckOkTech() == false) {
								label = label + " techtree";
							}
						}
						else {
							label += " - data synch is ok";
						}
					}
					else {
						if(serverInterface->getSlot(i,true) != NULL &&
						   serverInterface->getSlot(i,true)->getAllowGameDataSynchCheck() == true &&
						   serverInterface->getSlot(i,true)->getNetworkGameDataSynchCheckOk() == false) {
							label += " -synch mismatch:";

							if(serverInterface->getSlot(i,true) != NULL && serverInterface->getSlot(i,true)->getNetworkGameDataSynchCheckOkMap() == false) {
								label = label + " map";

								MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
								if(serverInterface->getSlot(i,true)->getReceivedDataSynchCheck() == true &&
									lastMapDataSynchError != "map CRC mismatch, " + cegui_manager.getSelectedItemFromComboBoxControl(
											cegui_manager.getControl("ComboMap"))) {

									lastMapDataSynchError = "map CRC mismatch, " + cegui_manager.getSelectedItemFromComboBoxControl(
											cegui_manager.getControl("ComboMap"));
									ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
									serverInterface->sendTextMessage(lastMapDataSynchError,-1, true,"");
								}
							}

							if(serverInterface->getSlot(i,true) != NULL &&
                               serverInterface->getSlot(i,true)->getNetworkGameDataSynchCheckOkTile() == false) {
								label = label + " tile";

								MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
								string selectedTileset = cegui_manager.getSelectedItemFromComboBoxControl(cegui_manager.getControl("ComboBoxTileset"));

								if(serverInterface->getSlot(i,true)->getReceivedDataSynchCheck() == true &&
									lastTileDataSynchError != "tile CRC mismatch, " + selectedTileset) {
									lastTileDataSynchError = "tile CRC mismatch, " + selectedTileset;
									ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
									serverInterface->sendTextMessage(lastTileDataSynchError,-1,true,"");
								}
							}

							if(serverInterface->getSlot(i,true) != NULL &&
                               serverInterface->getSlot(i,true)->getNetworkGameDataSynchCheckOkTech() == false) {
								label = label + " techtree";

								if(serverInterface->getSlot(i,true)->getReceivedDataSynchCheck() == true) {
									ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
									string report = serverInterface->getSlot(i,true)->getNetworkGameDataSynchCheckTechMismatchReport();

									if(lastTechtreeDataSynchError != "techtree CRC mismatch" + report) {
										lastTechtreeDataSynchError = "techtree CRC mismatch" + report;

										if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d] report: %s\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,report.c_str());

										serverInterface->sendTextMessage("techtree CRC mismatch",-1,true,"");
										vector<string> reportLineTokens;
										Tokenize(report,reportLineTokens,"\n");
										for(int reportLine = 0; reportLine < (int)reportLineTokens.size(); ++reportLine) {
											serverInterface->sendTextMessage(reportLineTokens[reportLine],-1,true,"");
										}
									}
								}
							}

							if(serverInterface->getSlot(i,true) != NULL) {
								serverInterface->getSlot(i,true)->setReceivedDataSynchCheck(false);
							}
						}
					}

					//float pingTime = serverInterface->getSlot(i)->getThreadedPingMS(serverInterface->getSlot(i)->getIpAddress().c_str());
					char szBuf[8096]="";
					snprintf(szBuf,8096,"%s",label.c_str());

					//labelNetStatus[i].setText(szBuf);
					MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
					cegui_manager.setControlText("LabelPlayer" + intToStr(i+1) + "Version",szBuf);
				}
				else {
					string port = "("+intToStr(config.getInt("PortServer"))+")";
					//labelNetStatus[i].setText("--- " + port);
					MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
					cegui_manager.setControlText("LabelPlayer" + intToStr(i+1) + "Version","--- " + port);
				}
			}
			else{
				//labelNetStatus[i].setText("");
				MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
				cegui_manager.setControlText("LabelPlayer" + intToStr(i+1) + "Version","");
			}
		}

		if(SystemFlags::getSystemSettingType(SystemFlags::debugPerformance).enabled && chrono.getMillis() > 0) SystemFlags::OutputDebug(SystemFlags::debugPerformance,"In [%s::%s Line: %d] took msecs: %lld\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,chrono.getMillis());
		if(SystemFlags::getSystemSettingType(SystemFlags::debugPerformance).enabled && chrono.getMillis() > 0) chrono.start();

		//ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();

		if(getAllowNetworkScenario() == false) {
			for(int i= 0; i< GameConstants::maxPlayers; ++i) {
				if(i >= mapInfo.players) {
					//listBoxControls[i].setEditable(false);
					//listBoxControls[i].setEnabled(false);
					setPlayerControlTypeEnabled(i,false);

					//printf("In [%s::%s] Line: %d i = %d mapInfo.players = %d\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,i,mapInfo.players);
				}
				else if(getSelectedPlayerControlTypeIndex(i) != ctNetworkUnassigned) {

					ConnectionSlot *slot = serverInterface->getSlot(i,true);
					if((getSelectedPlayerControlTypeIndex(i) != ctNetwork) ||
						(getSelectedPlayerControlTypeIndex(i) == ctNetwork && (slot == NULL || slot->isConnected() == false))) {

						//listBoxControls[i].setEditable(true);
						//listBoxControls[i].setEnabled(true);
						setPlayerControlTypeEnabled(i,true);
					}
					else {
						//listBoxControls[i].setEditable(false);
						//listBoxControls[i].setEnabled(false);
						setPlayerControlTypeEnabled(i,false);

						//printf("In [%s::%s] Line: %d i = %d mapInfo.players = %d\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,i,mapInfo.players);
					}
				}
				else {
					//listBoxControls[i].setEditable(false);
					//listBoxControls[i].setEnabled(false);
					setPlayerControlTypeEnabled(i,false);

					//printf("In [%s::%s] Line: %d i = %d mapInfo.players = %d\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,i,mapInfo.players);
				}
			}
		}

		bool checkDataSynch = (serverInterface->getAllowGameDataSynchCheck() == true &&
					needToSetChangedGameSettings == true &&
					(( difftime((long int)time(NULL),lastSetChangedGameSettings) >= BROADCAST_SETTINGS_SECONDS)||
					(this->headlessServerMode == true)));

		if(SystemFlags::getSystemSettingType(SystemFlags::debugPerformance).enabled && chrono.getMillis() > 0) SystemFlags::OutputDebug(SystemFlags::debugPerformance,"In [%s::%s Line: %d] took msecs: %lld\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,chrono.getMillis());
		if(SystemFlags::getSystemSettingType(SystemFlags::debugPerformance).enabled && chrono.getMillis() > 0) chrono.start();

		// Send the game settings to each client if we have at least one networked client
		if(checkDataSynch == true) {
			serverInterface->setGameSettings(&gameSettings,false);
			needToSetChangedGameSettings    = false;
		}

		if(SystemFlags::getSystemSettingType(SystemFlags::debugPerformance).enabled && chrono.getMillis() > 0) SystemFlags::OutputDebug(SystemFlags::debugPerformance,"In [%s::%s Line: %d] took msecs: %lld\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,chrono.getMillis());
		if(SystemFlags::getSystemSettingType(SystemFlags::debugPerformance).enabled && chrono.getMillis() > 0) chrono.start();

		//MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();

		if(this->headlessServerMode == true || hasOneNetworkSlotOpen == true ||
				getAllowInGameJoinPlayer() == true) {

			if(this->headlessServerMode == true &&
					GlobalStaticFlags::isFlagSet(gsft_lan_mode) == false) {
				//checkBoxPublishServer.setValue(true);

				cegui_manager.setCheckboxControlChecked(cegui_manager.getControl("CheckboxPublishGame"),true);
			}
			//listBoxFallbackCpuMultiplier.setEditable(true);
			cegui_manager.setControlEnabled(cegui_manager.getControl("SpinnerAIReplaceMultiplier"),true);
			//checkBoxPublishServer.setEditable(true);
			cegui_manager.setControlEnabled(cegui_manager.getControl("CheckboxPublishGame"),true);

			// Masterserver always needs one network slot
			if(this->headlessServerMode == true && hasOneNetworkSlotOpen == false) {
				bool anyoneConnected = false;
				for(int i= 0; i < mapInfo.players; ++i) {
					MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));

					ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
					ConnectionSlot *slot = serverInterface->getSlot(i,true);
					if(slot != NULL && slot->isConnected() == true) {
						anyoneConnected = true;
						break;
					}
				}

				for(int i= 0; i < mapInfo.players; ++i) {
					if(anyoneConnected == false && getSelectedPlayerControlTypeIndex(i) != ctNetwork) {
						//listBoxControls[i].setSelectedItemIndex(ctNetwork);
						setPlayerControlTypeSelectedIndex(i,ctNetwork);
					}
				}

				updateNetworkSlots();
			}
		}
		else {
			//checkBoxPublishServer.setValue(false);
			cegui_manager.setCheckboxControlChecked(cegui_manager.getControl("CheckboxPublishGame"),false);
			//checkBoxPublishServer.setEditable(false);
			cegui_manager.setControlEnabled(cegui_manager.getControl("CheckboxPublishGame"),false);

			//listBoxFallbackCpuMultiplier.setEditable(false);
			//listBoxFallbackCpuMultiplier.setSelectedItem("1.0");
			cegui_manager.setControlEnabled(cegui_manager.getControl("SpinnerAIReplaceMultiplier"),false);
			cegui_manager.setSpinnerControlValue(cegui_manager.getControl("SpinnerAIReplaceMultiplier"),1.0f);

			bool publishToServerEnabled =
					cegui_manager.getCheckboxControlChecked(
							cegui_manager.getControl("CheckboxPublishGame"));

            ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
            serverInterface->setPublishEnabled(publishToServerEnabled);
		}

		bool republishToMaster = (difftime((long int)time(NULL),lastMasterserverPublishing) >= MASTERSERVER_BROADCAST_PUBLISH_SECONDS);

		bool publishToServerEnabled =
				cegui_manager.getCheckboxControlChecked(
						cegui_manager.getControl("CheckboxPublishGame"));

		if(republishToMaster == true) {

			if(publishToServerEnabled) {
				needToRepublishToMasterserver = true;
				lastMasterserverPublishing = time(NULL);
			}
		}

		bool publishToServerEditable =
				cegui_manager.getControlEnabled(
						cegui_manager.getControl("CheckboxPublishGame"));

		bool callPublishNow = (publishToServerEditable &&
								publishToServerEnabled &&
									needToRepublishToMasterserver);

		if(SystemFlags::getSystemSettingType(SystemFlags::debugPerformance).enabled && chrono.getMillis() > 0) SystemFlags::OutputDebug(SystemFlags::debugPerformance,"In [%s::%s Line: %d] took msecs: %lld\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,chrono.getMillis());
		if(SystemFlags::getSystemSettingType(SystemFlags::debugPerformance).enabled && chrono.getMillis() > 0) chrono.start();

		if(callPublishNow == true) {
			// give it to me baby, aha aha ...
			publishToMasterserver();
		}
		if(needToPublishDelayed) {
			// this delay is done to make it possible to switch over maps which are not meant to be distributed
			if((difftime((long int)time(NULL), mapPublishingDelayTimer) >= BROADCAST_MAP_DELAY_SECONDS) ||
					(this->headlessServerMode == true)	){
				// after 5 seconds we are allowed to publish again!
				needToSetChangedGameSettings = true;
				lastSetChangedGameSettings   = time(NULL);
				// set to normal....
				needToPublishDelayed=false;
			}
		}
		if(needToPublishDelayed == false || headlessServerMode == true) {
			bool broadCastSettings = (difftime((long int)time(NULL),lastSetChangedGameSettings) >= BROADCAST_SETTINGS_SECONDS);

			//printf("broadCastSettings = %d\n",broadCastSettings);

			if(broadCastSettings == true) {
				needToBroadcastServerSettings=true;
				lastSetChangedGameSettings = time(NULL);
			}

			if(SystemFlags::getSystemSettingType(SystemFlags::debugPerformance).enabled && chrono.getMillis() > 0) SystemFlags::OutputDebug(SystemFlags::debugPerformance,"In [%s::%s Line: %d] took msecs: %lld\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,chrono.getMillis());
			if(SystemFlags::getSystemSettingType(SystemFlags::debugPerformance).enabled && chrono.getMillis() > 0) chrono.start();

			//broadCastSettings = (difftime(time(NULL),lastSetChangedGameSettings) >= 2);
			//if (broadCastSettings == true) {// reset timer here on bottom becasue used for different things
			//	lastSetChangedGameSettings = time(NULL);
			//}
		}

		//call the chat manager
		chatManager.updateNetwork();

		//console
		console.update();

		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

		if(SystemFlags::getSystemSettingType(SystemFlags::debugPerformance).enabled && chrono.getMillis() > 0) SystemFlags::OutputDebug(SystemFlags::debugPerformance,"In [%s::%s Line: %d] took msecs: %lld\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,chrono.getMillis());
		if(SystemFlags::getSystemSettingType(SystemFlags::debugPerformance).enabled && chrono.getMillis() > 0) chrono.start();

		if(currentConnectionCount > soundConnectionCount){
			soundConnectionCount = currentConnectionCount;
			SoundRenderer::getInstance().playFx(CoreData::getInstance().getAttentionSound());
			//switch on music again!!
			Config &config = Config::getInstance();
			float configVolume = (config.getInt("SoundVolumeMusic") / 100.f);
			CoreData::getInstance().getMenuMusic()->setVolume(configVolume);
		}
		soundConnectionCount = currentConnectionCount;

		if(SystemFlags::getSystemSettingType(SystemFlags::debugPerformance).enabled && chrono.getMillis() > 0) SystemFlags::OutputDebug(SystemFlags::debugPerformance,"In [%s::%s Line: %d] took msecs: %lld\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,chrono.getMillis());
		if(SystemFlags::getSystemSettingType(SystemFlags::debugPerformance).enabled && chrono.getMillis() > 0) chrono.start();

		if(enableFactionTexturePreview == true) {
			if( currentTechName_factionPreview != gameSettings.getTech() ||
				currentFactionName_factionPreview != gameSettings.getFactionTypeName(gameSettings.getThisFactionIndex())) {

				currentTechName_factionPreview=gameSettings.getTech();
				currentFactionName_factionPreview=gameSettings.getFactionTypeName(gameSettings.getThisFactionIndex());

				initFactionPreview(&gameSettings);
			}
		}

		if(SystemFlags::getSystemSettingType(SystemFlags::debugPerformance).enabled && chrono.getMillis() > 0) SystemFlags::OutputDebug(SystemFlags::debugPerformance,"In [%s::%s Line: %d] took msecs: %lld\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,chrono.getMillis());
		if(SystemFlags::getSystemSettingType(SystemFlags::debugPerformance).enabled && chrono.getMillis() > 0) chrono.start();

		if(autostart == true) {
			autostart = false;
			safeMutex.ReleaseLock();
			safeMutexCLI.ReleaseLock();
			if(autoStartSettings != NULL) {

				setupUIFromGameSettings(*autoStartSettings);
				ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
				serverInterface->setGameSettings(autoStartSettings,false);
			}
			else {
				RestoreLastGameSettings();
			}
			PlayNow((autoStartSettings == NULL));
			return;
		}
	}
    catch(megaglest_runtime_error& ex) {
    	//abort();
    	//printf("1111111bbbb ex.wantStackTrace() = %d\n",ex.wantStackTrace());
		char szBuf[8096]="";
		snprintf(szBuf,8096,"In [%s::%s %d]\nError detected:\n%s\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,ex.what());
		SystemFlags::OutputDebug(SystemFlags::debugError,szBuf);
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s",szBuf);

		//printf("2222222bbbb ex.wantStackTrace() = %d\n",ex.wantStackTrace());

		showGeneralError=true;
		generalErrorToShow = szBuf;
    }
	catch(const std::exception &ex) {
		char szBuf[8096]="";
		snprintf(szBuf,8096,"In [%s::%s %d]\nError detected:\n%s\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,ex.what());
		SystemFlags::OutputDebug(SystemFlags::debugError,szBuf);
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s",szBuf);

		showGeneralError=true;
		generalErrorToShow = szBuf;
	}

	// Must release locks in case delayed callbacks delete this window
	safeMutex.ReleaseLock();
	safeMutexCLI.ReleaseLock();
	MenuState::update();
}

void MenuStateCustomGame::initFactionPreview(const GameSettings *gameSettings) {
	string factionVideoUrl = "";
	string factionVideoUrlFallback = "";

	string factionDefinitionXML = Game::findFactionLogoFile(gameSettings, NULL,currentFactionName_factionPreview + ".xml");
	if(factionDefinitionXML != "" && currentFactionName_factionPreview != GameConstants::RANDOMFACTION_SLOTNAME &&
			currentFactionName_factionPreview != GameConstants::OBSERVER_SLOTNAME && fileExists(factionDefinitionXML) == true) {
		XmlTree	xmlTree;
		std::map<string,string> mapExtraTagReplacementValues;
		xmlTree.load(factionDefinitionXML, Properties::getTagReplacementValues(&mapExtraTagReplacementValues));
		const XmlNode *factionNode= xmlTree.getRootNode();
		if(factionNode->hasAttribute("faction-preview-video") == true) {
			factionVideoUrl = factionNode->getAttribute("faction-preview-video")->getValue();
		}

		factionVideoUrlFallback = Game::findFactionLogoFile(gameSettings, NULL,"preview_video.*");
		if(factionVideoUrl == "") {
			factionVideoUrl = factionVideoUrlFallback;
			factionVideoUrlFallback = "";
		}
	}
	//printf("currentFactionName_factionPreview [%s] random [%s] observer [%s] factionVideoUrl [%s]\n",currentFactionName_factionPreview.c_str(),GameConstants::RANDOMFACTION_SLOTNAME,GameConstants::OBSERVER_SLOTNAME,factionVideoUrl.c_str());


	if(factionVideoUrl != "") {
		//SoundRenderer &soundRenderer= SoundRenderer::getInstance();
		if(CoreData::getInstance().getMenuMusic()->getVolume() != 0) {
			CoreData::getInstance().getMenuMusic()->setVolume(0);
			factionVideoSwitchedOffVolume=true;
		}

		if(currentFactionLogo != factionVideoUrl) {
			currentFactionLogo = factionVideoUrl;
			if(GlobalStaticFlags::getIsNonGraphicalModeEnabled() == false &&
					::Shared::Graphics::VideoPlayer::hasBackEndVideoPlayer() == true) {

				if(factionVideo != NULL) {
					factionVideo->closePlayer();
					delete factionVideo;
					factionVideo = NULL;
				}
				string introVideoFile = factionVideoUrl;
				string introVideoFileFallback = factionVideoUrlFallback;

				Context *c= GraphicsInterface::getInstance().getCurrentContext();
				SDL_Surface *screen = static_cast<ContextGl*>(c)->getPlatformContextGlPtr()->getScreen();

				string vlcPluginsPath = Config::getInstance().getString("VideoPlayerPluginsPath","");
				//printf("screen->w = %d screen->h = %d screen->format->BitsPerPixel = %d\n",screen->w,screen->h,screen->format->BitsPerPixel);
				factionVideo = new VideoPlayer(
						&Renderer::getInstance(),
						introVideoFile,
						introVideoFileFallback,
						screen,
						0,0,
						screen->w,
						screen->h,
						screen->format->BitsPerPixel,
						true,
						vlcPluginsPath,
						SystemFlags::VERBOSE_MODE_ENABLED);
				factionVideo->initPlayer();
			}
		}
	}
	else {
		//SoundRenderer &soundRenderer= SoundRenderer::getInstance();
		//switch on music again!!
		Config &config = Config::getInstance();
		float configVolume = (config.getInt("SoundVolumeMusic") / 100.f);
		if(factionVideoSwitchedOffVolume){
			if(CoreData::getInstance().getMenuMusic()->getVolume() != configVolume) {
				CoreData::getInstance().getMenuMusic()->setVolume(configVolume);
			}
			factionVideoSwitchedOffVolume=false;
		}

		if(factionVideo != NULL) {
			factionVideo->closePlayer();
			delete factionVideo;
			factionVideo = NULL;
		}
	}

	if(factionVideo == NULL) {
		string factionLogo = Game::findFactionLogoFile(gameSettings, NULL,GameConstants::PREVIEW_SCREEN_FILE_FILTER);
		if(factionLogo == "") {
			factionLogo = Game::findFactionLogoFile(gameSettings, NULL);
		}
		if(currentFactionLogo != factionLogo) {
			currentFactionLogo = factionLogo;
			loadFactionTexture(currentFactionLogo);
		}
	}
}

void MenuStateCustomGame::publishToMasterserver() {
	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	int slotCountUsed				 = 0;
	int slotCountHumans				 = 0;
	int slotCountConnectedPlayers	 = 0;
	ServerInterface *serverInterface = NetworkManager::getInstance().getServerInterface();

	GameSettings gameSettings;
	loadGameSettings(&gameSettings);
	Config &config						  = Config::getInstance();
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();

	//string serverinfo="";

	MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));

	publishToServerInfo.clear();

	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	for(int i= 0; i < mapInfo.players; ++i) {
		if(getSelectedPlayerControlTypeIndex(i) != ctClosed) {
			slotCountUsed++;
		}

		if(getSelectedPlayerControlTypeIndex(i) == ctNetwork ||
				getSelectedPlayerControlTypeIndex(i) == ctNetworkUnassigned)
		{
			slotCountHumans++;
			if(serverInterface->getSlot(i,true) != NULL &&
               serverInterface->getSlot(i,true)->isConnected()) {
				slotCountConnectedPlayers++;
			}
		}
		else if(getSelectedPlayerControlTypeIndex(i) == ctHuman) {
			slotCountHumans++;
			slotCountConnectedPlayers++;
		}
	}

	publishToServerInfo["uuid"] = Config::getInstance().getString("PlayerId","");

	//?status=waiting&system=linux&info=titus
	publishToServerInfo["glestVersion"] = glestVersionString;
	publishToServerInfo["platform"] = getPlatformNameString() + "-" + getGITRevisionString();
    publishToServerInfo["binaryCompileDate"] = getCompileDateTime();

	//game info:
	publishToServerInfo["serverTitle"] = getHumanPlayerName() + "'s game";
	//publishToServerInfo["serverTitle"] = labelGameName.getText();

	publishToServerInfo["serverTitle"] = cegui_manager.getControlText("EditboxPublishGameName");

	//ip is automatically set

	//game setup info:

	int selectedTechtreeIndex = cegui_manager.getSelectedItemIndexFromComboBoxControl(cegui_manager.getControl("ComboBoxTechTree"));
	if(selectedTechtreeIndex < 0) {
		return;
	}
	//publishToServerInfo["tech"] = listBoxTechTree.getSelectedItem();

	//printf("Line: %d (%d)\n",__LINE__,selectedTechtreeIndex);
    publishToServerInfo["tech"] = techTreeFiles.at(selectedTechtreeIndex);
	//publishToServerInfo["map"] = listBoxMap.getSelectedItem();
    publishToServerInfo["map"] = getCurrentMapFile();
	//publishToServerInfo["tileset"] = listBoxTileset.getSelectedItem();

	int selectedTilesetIndex = cegui_manager.getSelectedItemIndexFromComboBoxControl(cegui_manager.getControl("ComboBoxTileset"));
	if(selectedTilesetIndex < 0) {
		return;
	}
	publishToServerInfo["tileset"] = tilesetFiles.at(selectedTilesetIndex);

	publishToServerInfo["activeSlots"] = intToStr(slotCountUsed);
	publishToServerInfo["networkSlots"] = intToStr(slotCountHumans);
	publishToServerInfo["connectedClients"] = intToStr(slotCountConnectedPlayers);

	string serverPort=config.getString("PortServer", intToStr(GameConstants::serverPort).c_str());
	string externalPort=config.getString("PortExternal", serverPort.c_str());
	publishToServerInfo["externalconnectport"] = externalPort;
	publishToServerInfo["privacyPlease"] = intToStr(config.getBool("PrivacyPlease","false"));

	publishToServerInfo["gameStatus"] = intToStr(game_status_waiting_for_players);
	if(slotCountHumans <= slotCountConnectedPlayers) {
		publishToServerInfo["gameStatus"] = intToStr(game_status_waiting_for_start);
	}

	publishToServerInfo["gameUUID"] = gameSettings.getGameUUID();
	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
}

void MenuStateCustomGame::setupTask(BaseThread *callingThread,void *userdata) {
	if(SystemFlags::VERBOSE_MODE_ENABLED) printf("\n\nsetupTask callingThread [%p] userdata [%p]\n",callingThread,userdata);
	if(userdata != NULL) {
		int value = *((int*)&userdata);
		THREAD_NOTIFIER_TYPE threadType = (THREAD_NOTIFIER_TYPE)value;
		//printf("\n\nsetupTask callingThread [%p] userdata [%p]\n",callingThread,userdata);
		if(threadType == tnt_MASTERSERVER) {
			MenuStateCustomGame::setupTaskStatic(callingThread);
		}
	}
}
void MenuStateCustomGame::shutdownTask(BaseThread *callingThread,void *userdata) {
	//printf("\n\nshutdownTask callingThread [%p] userdata [%p]\n",callingThread,userdata);
	if(SystemFlags::VERBOSE_MODE_ENABLED) printf("\n\nshutdownTask callingThread [%p] userdata [%p]\n",callingThread,userdata);
	if(userdata != NULL) {
		int value = *((int*)&userdata);
		THREAD_NOTIFIER_TYPE threadType = (THREAD_NOTIFIER_TYPE)value;
		//printf("\n\nshutdownTask callingThread [%p] userdata [%p]\n",callingThread,userdata);
		if(threadType == tnt_MASTERSERVER) {
			MenuStateCustomGame::shutdownTaskStatic(callingThread);
		}
	}
}
void MenuStateCustomGame::setupTaskStatic(BaseThread *callingThread) {
	if(SystemFlags::VERBOSE_MODE_ENABLED) printf("In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	CURL *handle = SystemFlags::initHTTP();
	callingThread->setGenericData<CURL>(handle);

	if(SystemFlags::VERBOSE_MODE_ENABLED) printf("In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
}
void MenuStateCustomGame::shutdownTaskStatic(BaseThread *callingThread) {
	if(SystemFlags::VERBOSE_MODE_ENABLED) printf("In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	//printf("LINE: %d\n",__LINE__);
	CURL *handle = callingThread->getGenericData<CURL>();
	SystemFlags::cleanupHTTP(&handle);

	if(SystemFlags::VERBOSE_MODE_ENABLED) printf("In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
}

void MenuStateCustomGame::simpleTask(BaseThread *callingThread,void *userdata) {
	//printf("\n\nSimple Task callingThread [%p] userdata [%p]\n",callingThread,userdata);
	int value = *((int*)&userdata);
	//printf("\n\nSimple Task callingThread [%p] userdata [%p] value = %d\n",callingThread,userdata,value);

	THREAD_NOTIFIER_TYPE threadType = (THREAD_NOTIFIER_TYPE)value;
	if(threadType == tnt_MASTERSERVER) {
		simpleTaskForMasterServer(callingThread);
	}
	else if(threadType == tnt_CLIENTS) {
		simpleTaskForClients(callingThread);
	}
}

void MenuStateCustomGame::simpleTaskForMasterServer(BaseThread *callingThread) {
    try {
        //printf("-=-=-=-=- IN MenuStateCustomGame simpleTask - A\n");

        MutexSafeWrapper safeMutexThreadOwner(callingThread->getMutexThreadOwnerValid(),string(__FILE__) + "_" + intToStr(__LINE__));
        if(callingThread->getQuitStatus() == true || safeMutexThreadOwner.isValidMutex() == false) {
            return;
        }

        //printf("-=-=-=-=- IN MenuStateCustomGame simpleTask - B\n");

        MutexSafeWrapper safeMutex(callingThread->getMutexThreadObjectAccessor(),string(__FILE__) + "_" + intToStr(__LINE__));
        bool republish                                  = (needToRepublishToMasterserver == true  && publishToServerInfo.empty() == false);
        needToRepublishToMasterserver                   = false;
        std::map<string,string> newPublishToServerInfo  = publishToServerInfo;
        publishToServerInfo.clear();

        //printf("simpleTask broadCastSettings = %d\n",broadCastSettings);

        if(callingThread->getQuitStatus() == true) {
            return;
        }

        //printf("-=-=-=-=- IN MenuStateCustomGame simpleTask - C\n");

        if(republish == true) {
        	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

            string request = Config::getInstance().getString("Masterserver");
    		if(request != "") {
    			endPathWithSlash(request,false);
    		}
    		request += "addServerInfo.php?";

            //CURL *handle = SystemFlags::initHTTP();
            CURL *handle = callingThread->getGenericData<CURL>();

            int paramIndex = 0;
            for(std::map<string,string>::const_iterator iterMap = newPublishToServerInfo.begin();
                iterMap != newPublishToServerInfo.end(); ++iterMap) {

                request += iterMap->first;
                request += "=";
                request += SystemFlags::escapeURL(iterMap->second,handle);

                paramIndex++;
                if(paramIndex < (int)newPublishToServerInfo.size()) {
                	request += "&";
                }
            }

            if(SystemFlags::VERBOSE_MODE_ENABLED) printf("The Lobby request is:\n%s\n",request.c_str());

            if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d] the request is:\n%s\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,request.c_str());
            safeMutex.ReleaseLock(true);
            safeMutexThreadOwner.ReleaseLock();

            std::string serverInfo = SystemFlags::getHTTP(request,handle);
            //SystemFlags::cleanupHTTP(&handle);

            MutexSafeWrapper safeMutexThreadOwner2(callingThread->getMutexThreadOwnerValid(),string(__FILE__) + "_" + intToStr(__LINE__));
            if(callingThread->getQuitStatus() == true || safeMutexThreadOwner2.isValidMutex() == false) {
                return;
            }
            safeMutex.Lock();

            //printf("the result is:\n'%s'\n",serverInfo.c_str());
            if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d] the result is:\n'%s'\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,serverInfo.c_str());

            // uncomment to enable router setup check of this server
            if(EndsWith(serverInfo, "OK") == false) {
                if(callingThread->getQuitStatus() == true) {
                    return;
                }

                // Give things another chance to see if we can get a connection from the master server
                if(tMasterserverErrorElapsed > 0 &&
                		difftime((long int)time(NULL),tMasterserverErrorElapsed) > MASTERSERVER_BROADCAST_MAX_WAIT_RESPONSE_SECONDS) {
                	showMasterserverError=true;
                	masterServererErrorToShow = (serverInfo != "" ? serverInfo : "No Reply");
                }
                else {
                	if(tMasterserverErrorElapsed == 0) {
                		tMasterserverErrorElapsed = time(NULL);
                	}

                	SystemFlags::OutputDebug(SystemFlags::debugError,"In [%s::%s Line %d] error checking response from masterserver elapsed seconds = %.2f / %d\nResponse:\n%s\n",
                			extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,difftime((long int)time(NULL),tMasterserverErrorElapsed),MASTERSERVER_BROADCAST_MAX_WAIT_RESPONSE_SECONDS,serverInfo.c_str());

                	needToRepublishToMasterserver = true;
                }
            }
        }
        else {
            safeMutexThreadOwner.ReleaseLock();
        }

        if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

        //printf("-=-=-=-=- IN MenuStateCustomGame simpleTask - D\n");

        safeMutex.ReleaseLock();

        //printf("-=-=-=-=- IN MenuStateCustomGame simpleTask - F\n");
    }
	catch(const std::exception &ex) {
		char szBuf[8096]="";
		snprintf(szBuf,8096,"In [%s::%s %d]\nError detected:\n%s\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,ex.what());
		SystemFlags::OutputDebug(SystemFlags::debugError,szBuf);
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s",szBuf);

		if(callingThread->getQuitStatus() == false) {
            //throw megaglest_runtime_error(szBuf);
            showGeneralError=true;
            generalErrorToShow = ex.what();
		}
	}

	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
}

void MenuStateCustomGame::simpleTaskForClients(BaseThread *callingThread) {
    try {
        //printf("-=-=-=-=- IN MenuStateCustomGame simpleTask - A\n");

        MutexSafeWrapper safeMutexThreadOwner(callingThread->getMutexThreadOwnerValid(),string(__FILE__) + "_" + intToStr(__LINE__));
        if(callingThread->getQuitStatus() == true || safeMutexThreadOwner.isValidMutex() == false) {
            return;
        }

        //printf("-=-=-=-=- IN MenuStateCustomGame simpleTask - B\n");

        MutexSafeWrapper safeMutex(callingThread->getMutexThreadObjectAccessor(),string(__FILE__) + "_" + intToStr(__LINE__));
        bool broadCastSettings                          = needToBroadcastServerSettings;

        //printf("simpleTask broadCastSettings = %d\n",broadCastSettings);

        needToBroadcastServerSettings                   = false;
        bool hasClientConnection                        = false;

        if(broadCastSettings == true) {
            ServerInterface *serverInterface = NetworkManager::getInstance().getServerInterface(false);
            if(serverInterface != NULL) {
                hasClientConnection = serverInterface->hasClientConnection();
            }
        }
        bool needPing = (difftime((long int)time(NULL),lastNetworkPing) >= GameConstants::networkPingInterval);

        if(callingThread->getQuitStatus() == true) {
            return;
        }

        //printf("-=-=-=-=- IN MenuStateCustomGame simpleTask - C\n");

        safeMutexThreadOwner.ReleaseLock();

        if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

        //printf("-=-=-=-=- IN MenuStateCustomGame simpleTask - D\n");

        if(broadCastSettings == true) {
            MutexSafeWrapper safeMutexThreadOwner2(callingThread->getMutexThreadOwnerValid(),string(__FILE__) + "_" + intToStr(__LINE__));
            if(callingThread->getQuitStatus() == true || safeMutexThreadOwner2.isValidMutex() == false) {
                return;
            }

            if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

            //printf("simpleTask broadCastSettings = %d hasClientConnection = %d\n",broadCastSettings,hasClientConnection);

            if(callingThread->getQuitStatus() == true) {
                return;
            }
            ServerInterface *serverInterface= NetworkManager::getInstance().getServerInterface(false);
            if(serverInterface != NULL) {

            	if(this->headlessServerMode == false || (serverInterface->getGameSettingsUpdateCount() <= lastMasterServerSettingsUpdateCount)) {
                    GameSettings gameSettings;
                    loadGameSettings(&gameSettings);

                    //printf("\n\n\n\n=====#2 got settings [%d] [%d]:\n%s\n",lastMasterServerSettingsUpdateCount,serverInterface->getGameSettingsUpdateCount(),gameSettings.toString().c_str());

                    if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

					serverInterface->setGameSettings(&gameSettings,false);
					lastMasterServerSettingsUpdateCount = serverInterface->getGameSettingsUpdateCount();

					if(hasClientConnection == true) {
						if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
						serverInterface->broadcastGameSetup(&gameSettings);
					}
            	}
            }
        }

        //printf("-=-=-=-=- IN MenuStateCustomGame simpleTask - E\n");

        if(needPing == true) {
            MutexSafeWrapper safeMutexThreadOwner2(callingThread->getMutexThreadOwnerValid(),string(__FILE__) + "_" + intToStr(__LINE__));
            if(callingThread->getQuitStatus() == true || safeMutexThreadOwner2.isValidMutex() == false) {
                return;
            }

            lastNetworkPing = time(NULL);

            if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d] Sending nmtPing to clients\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

            ServerInterface *serverInterface= NetworkManager::getInstance().getServerInterface(false);
            if(serverInterface != NULL) {
            	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
                NetworkMessagePing *msg = new NetworkMessagePing(GameConstants::networkPingInterval,time(NULL));
                //serverInterface->broadcastPing(&msg);
                serverInterface->queueBroadcastMessage(msg);
                if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
            }
        }
        safeMutex.ReleaseLock();

        //printf("-=-=-=-=- IN MenuStateCustomGame simpleTask - F\n");
    }
	catch(const std::exception &ex) {
		char szBuf[8096]="";
		snprintf(szBuf,8096,"In [%s::%s %d]\nError detected:\n%s\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,ex.what());
		SystemFlags::OutputDebug(SystemFlags::debugError,szBuf);
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s",szBuf);

		if(callingThread->getQuitStatus() == false) {
            //throw megaglest_runtime_error(szBuf);
            showGeneralError=true;
            generalErrorToShow = ex.what();
		}
	}

	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
}

void MenuStateCustomGame::loadGameSettings(GameSettings *gameSettings,bool forceCloseUnusedSlots) {
	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	int factionCount= 0;
	ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();

	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s] Line: %d\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	if(this->headlessServerMode == true && serverInterface->getGameSettingsUpdateCount() > lastMasterServerSettingsUpdateCount &&
			serverInterface->getGameSettings() != NULL) {
		const GameSettings *settings = serverInterface->getGameSettings();
		//printf("\n\n\n\n=====#3 got settings [%d] [%d]:\n%s\n",lastMasterServerSettingsUpdateCount,serverInterface->getGameSettingsUpdateCount(),settings->toString().c_str());

		lastMasterServerSettingsUpdateCount = serverInterface->getGameSettingsUpdateCount();
		//printf("#1 custom menu got map [%s]\n",settings->getMap().c_str());

		setupUIFromGameSettings(*settings);
	}

    // Test flags values
    //gameSettings->setFlagTypes1(ft1_show_map_resources);
    //

	if(getAllowNetworkScenario() == true) {
		gameSettings->setScenario(scenarioInfo.name);
		gameSettings->setScenarioDir(Scenario::getScenarioPath(dirList, scenarioInfo.name));

		gameSettings->setDefaultResources(scenarioInfo.defaultResources);
		gameSettings->setDefaultUnits(scenarioInfo.defaultUnits);
		gameSettings->setDefaultVictoryConditions(scenarioInfo.defaultVictoryConditions);
	}
	else {
		gameSettings->setScenario("");
		gameSettings->setScenarioDir("");
	}

	gameSettings->setGameUUID(this->gameUUID);

	//printf("scenarioInfo.name [%s] [%s] [%s]\n",scenarioInfo.name.c_str(),listBoxMap.getSelectedItem().c_str(),getCurrentMapFile().c_str());

	//gameSettings->setMapFilterIndex(listBoxMapFilter.getSelectedItemIndex());
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	gameSettings->setMapFilterIndex(cegui_manager.getSelectedItemIndexFromComboBoxControl(cegui_manager.getControl("ComboMapFilter")));

	gameSettings->setDescription(formatString(getCurrentMapFile()));
	gameSettings->setMap(getCurrentMapFile());
	if(tilesetFiles.empty() == false) {
		int selectedTilesetIndex = cegui_manager.getSelectedItemIndexFromComboBoxControl(cegui_manager.getControl("ComboBoxTileset"));
		if(selectedTilesetIndex >= 0) {
			gameSettings->setTileset(tilesetFiles.at(selectedTilesetIndex));
		}
	}
	if(techTreeFiles.empty() == false) {
		MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
		int selectedTechtreeIndex = cegui_manager.getSelectedItemIndexFromComboBoxControl(cegui_manager.getControl("ComboBoxTechTree"));

		//printf("Line: %d (%d)\n",__LINE__,selectedTechtreeIndex);
		if(selectedTechtreeIndex >= 0) {
			gameSettings->setTech(techTreeFiles.at(selectedTechtreeIndex));
		}
	}

    if(autoStartSettings != NULL) {
    	gameSettings->setDefaultUnits(autoStartSettings->getDefaultUnits());
    	gameSettings->setDefaultResources(autoStartSettings->getDefaultResources());
    	gameSettings->setDefaultVictoryConditions(autoStartSettings->getDefaultVictoryConditions());
    }
    else if(getAllowNetworkScenario() == false) {
		gameSettings->setDefaultUnits(true);
		gameSettings->setDefaultResources(true);
		gameSettings->setDefaultVictoryConditions(true);
    }

	int selectedFogOfWarIndex = cegui_manager.getSelectedItemIndexFromComboBoxControl(cegui_manager.getControl("ComboBoxFogOfWar"));

   	gameSettings->setFogOfWar(selectedFogOfWarIndex == 0 ||
   			selectedFogOfWarIndex == 1 );

	gameSettings->setAllowObservers(getAllowObservers());

	uint32 valueFlags1 = gameSettings->getFlagTypes1();
	if(selectedFogOfWarIndex == 1 ||
			selectedFogOfWarIndex == 2 ) {

        valueFlags1 |= ft1_show_map_resources;
        gameSettings->setFlagTypes1(valueFlags1);
	}
	else {
        valueFlags1 &= ~ft1_show_map_resources;
        gameSettings->setFlagTypes1(valueFlags1);
	}

	//gameSettings->setEnableObserverModeAtEndGame(listBoxEnableObserverMode.getSelectedItemIndex() == 0);
	gameSettings->setEnableObserverModeAtEndGame(true);
	//gameSettings->setPathFinderType(static_cast<PathFinderType>(listBoxPathFinderType.getSelectedItemIndex()));

	valueFlags1 = gameSettings->getFlagTypes1();
	if(getAllowSwitchTeams() == true) {
        valueFlags1 |= ft1_allow_team_switching;
        gameSettings->setFlagTypes1(valueFlags1);
	}
	else {
        valueFlags1 &= ~ft1_allow_team_switching;
        gameSettings->setFlagTypes1(valueFlags1);
	}
	//gameSettings->setAiAcceptSwitchTeamPercentChance(strToInt(listBoxAISwitchTeamAcceptPercent.getSelectedItem()));
	//MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl("ComboAIAcceptPercent");
	gameSettings->setAiAcceptSwitchTeamPercentChance(strToInt(cegui_manager.getSelectedItemFromComboBoxControl(ctl)));

	//gameSettings->setFallbackCpuMultiplier(listBoxFallbackCpuMultiplier.getSelectedItemIndex());
	//cegui_manager.setControlEnabled(cegui_manager.getControl("SpinnerAIReplaceMultiplier"),false);
	//cegui_manager.setSelectedItemInComboBoxControl(cegui_manager.getControl("SpinnerAIReplaceMultiplier"),"1.0");
	cegui_manager.setSpinnerControlValue(cegui_manager.getControl("SpinnerAIReplaceMultiplier"),1.0f);

	gameSettings->setFallbackCpuMultiplier(cegui_manager.getSpinnerControlValue(cegui_manager.getControl("SpinnerAIReplaceMultiplier")));

	if(getAllowInGameJoinPlayer() == true) {
        valueFlags1 |= ft1_allow_in_game_joining;
        gameSettings->setFlagTypes1(valueFlags1);
	}
	else {
        valueFlags1 &= ~ft1_allow_in_game_joining;
        gameSettings->setFlagTypes1(valueFlags1);
	}

	if(getAllowTeamUnitSharing() == true) {
        valueFlags1 |= ft1_allow_shared_team_units;
        gameSettings->setFlagTypes1(valueFlags1);
	}
	else {
        valueFlags1 &= ~ft1_allow_shared_team_units;
        gameSettings->setFlagTypes1(valueFlags1);
	}

	if(getAllowTeamResourceSharing() == true) {
        valueFlags1 |= ft1_allow_shared_team_resources;
        gameSettings->setFlagTypes1(valueFlags1);
	}
	else {
        valueFlags1 &= ~ft1_allow_shared_team_resources;
        gameSettings->setFlagTypes1(valueFlags1);
	}

	if(Config::getInstance().getBool("EnableNetworkGameSynchChecks","false") == true) {
		//printf("*WARNING* - EnableNetworkGameSynchChecks is enabled\n");

        valueFlags1 |= ft1_network_synch_checks_verbose;
        gameSettings->setFlagTypes1(valueFlags1);

	}
	else {
        valueFlags1 &= ~ft1_network_synch_checks_verbose;
        gameSettings->setFlagTypes1(valueFlags1);

	}
	if(Config::getInstance().getBool("EnableNetworkGameSynchMonitor","false") == true) {
		//printf("*WARNING* - EnableNetworkGameSynchChecks is enabled\n");

        valueFlags1 |= ft1_network_synch_checks;
        gameSettings->setFlagTypes1(valueFlags1);

	}
	else {
        valueFlags1 &= ~ft1_network_synch_checks;
        gameSettings->setFlagTypes1(valueFlags1);

	}

	gameSettings->setNetworkAllowNativeLanguageTechtree(getAllowNativeLanguageTechtree());

	// First save Used slots
    //for(int i=0; i<mapInfo.players; ++i)
	int AIPlayerCount = 0;
	for(int i = 0; i < GameConstants::maxPlayers; ++i) {
		if (getSelectedPlayerControlTypeIndex(i) == ctHuman && this->headlessServerMode == true) {
			// switch slot to network, because no human in headless mode
			//listBoxControls[i].setSelectedItemIndex(ctNetwork) ;
			setPlayerControlTypeSelectedIndex(i,ctNetwork);
			updateResourceMultiplier(i);
		}

		ControlType ct= static_cast<ControlType>(getSelectedPlayerControlTypeIndex(i));

		if(forceCloseUnusedSlots == true && (ct == ctNetworkUnassigned || ct == ctNetwork)) {
			if(serverInterface != NULL &&
			   (serverInterface->getSlot(i,true) == NULL ||
               serverInterface->getSlot(i,true)->isConnected() == false)) {
				if(getAllowNetworkScenario() == false) {
				//printf("Closed A [%d] [%s]\n",i,labelPlayerNames[i].getText().c_str());

					//listBoxControls[i].setSelectedItemIndex(ctClosed);
					setPlayerControlTypeSelectedIndex(i,ctClosed);
					ct = ctClosed;
				}
			}
		}
		else if(ct == ctNetworkUnassigned && i < mapInfo.players) {
			//listBoxControls[i].setSelectedItemIndex(ctNetwork);
			setPlayerControlTypeSelectedIndex(i,ctNetwork);
			ct = ctNetwork;
		}

		if(ct != ctClosed) {
			int slotIndex = factionCount;
			gameSettings->setFactionControl(slotIndex, ct);
			if(ct == ctHuman) {
				if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d] i = %d, slotIndex = %d, getHumanPlayerName(i) [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,i,slotIndex,getHumanPlayerName(i).c_str());

				gameSettings->setThisFactionIndex(slotIndex);
				gameSettings->setNetworkPlayerName(slotIndex, getHumanPlayerName(i));
				gameSettings->setNetworkPlayerUUID(slotIndex,Config::getInstance().getString("PlayerId",""));
				gameSettings->setNetworkPlayerPlatform(slotIndex,getPlatformNameString());
				gameSettings->setNetworkPlayerStatuses(slotIndex, getNetworkPlayerStatus());
				Lang &lang= Lang::getInstance();
				gameSettings->setNetworkPlayerLanguages(slotIndex, lang.getLanguage());
			}
			else if(serverInterface != NULL && serverInterface->getSlot(i,true) != NULL) {
				gameSettings->setNetworkPlayerLanguages(slotIndex, serverInterface->getSlot(i,true)->getNetworkPlayerLanguage());
			}

			//if(slotIndex == 0) printf("slotIndex = %d, i = %d, multiplier = %d\n",slotIndex,i,listBoxRMultiplier[i].getSelectedItemIndex());

			//printf("Line: %d multiplier index: %d slotIndex: %d\n",__LINE__,listBoxRMultiplier[i].getSelectedItemIndex(),slotIndex);
			//gameSettings->setResourceMultiplierIndex(slotIndex, listBoxRMultiplier[i].getSelectedItemIndex());
			gameSettings->setResourceMultiplierIndex(slotIndex,
					convertMultiplierValueToIndex(cegui_manager.getSpinnerControlValue(
							cegui_manager.getControl(
									"SpinnerPlayer" + intToStr(i+1) + "Multiplier"))));


			//printf("Line: %d multiplier index: %d slotIndex: %d\n",__LINE__,gameSettings->getResourceMultiplierIndex(slotIndex),slotIndex);

			//if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d] i = %d, factionFiles[listBoxFactions[i].getSelectedItemIndex()] [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,i,factionFiles[listBoxFactions[i].getSelectedItemIndex()].c_str());

			//gameSettings->setFactionTypeName(slotIndex, factionFiles[listBoxFactions[i].getSelectedItemIndex()]);
			int selectedFactionIndex = getSelectedPlayerFactionTypeIndex(i);
			if(selectedFactionIndex >= 0) {
				gameSettings->setFactionTypeName(slotIndex, factionFiles.at(selectedFactionIndex));

				//if(factionFiles[listBoxFactions[i].getSelectedItemIndex()] == formatString(GameConstants::OBSERVER_SLOTNAME)) {
				if(factionFiles.at(selectedFactionIndex) == formatString(GameConstants::OBSERVER_SLOTNAME)) {

					setSelectedPlayerTeam(i,intToStr(GameConstants::maxPlayers + fpt_Observer));
				}
				else if(getPlayerTeamSelectedItem(i) == intToStr(GameConstants::maxPlayers + fpt_Observer)) {

					//printf("Line: %d lastSelectedTeamIndex[i] = %d \n",__LINE__,lastSelectedTeamIndex[i]);

					if((getSelectedPlayerControlTypeIndex(i) == ctCpuEasy || getSelectedPlayerControlTypeIndex(i) == ctCpu ||
							getSelectedPlayerControlTypeIndex(i) == ctCpuUltra || getSelectedPlayerControlTypeIndex(i) == ctCpuMega) &&
							getAllowNetworkScenario() == true) {

					}
					else {
						if(lastSelectedTeamIndex[i] >= 0 && lastSelectedTeamIndex[i] + 1 != (GameConstants::maxPlayers + fpt_Observer)) {
							if(lastSelectedTeamIndex[i] == 0) {
								lastSelectedTeamIndex[i] = GameConstants::maxPlayers-1;
							}
							else if(lastSelectedTeamIndex[i] == GameConstants::maxPlayers-1) {
								lastSelectedTeamIndex[i] = 0;
							}

							setSelectedPlayerTeamIndex(i,lastSelectedTeamIndex[i]);
						}
						else {
							setSelectedPlayerTeam(i,intToStr(1));
						}
					}
				}
			}

			gameSettings->setTeam(slotIndex, getSelectedPlayerTeamIndex(i));
			gameSettings->setStartLocationIndex(slotIndex, i);

			if(getSelectedPlayerControlTypeIndex(i) == ctNetwork ||
					getSelectedPlayerControlTypeIndex(i) == ctNetworkUnassigned) {
				if(serverInterface != NULL &&
					serverInterface->getSlot(i,true) != NULL &&
                   serverInterface->getSlot(i,true)->isConnected()) {

					gameSettings->setNetworkPlayerStatuses(slotIndex,serverInterface->getSlot(i,true)->getNetworkPlayerStatus());

					if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d] i = %d, connectionSlot->getName() [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,i,serverInterface->getSlot(i,true)->getName().c_str());

					gameSettings->setNetworkPlayerName(slotIndex, serverInterface->getSlot(i,true)->getName());
					gameSettings->setNetworkPlayerUUID(i,serverInterface->getSlot(i,true)->getUUID());
					gameSettings->setNetworkPlayerPlatform(i,serverInterface->getSlot(i,true)->getPlatform());
					setPlayerNameText(i,serverInterface->getSlot(i,true)->getName());
				}
				else {
					if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d] i = %d, playername unconnected\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,i);

					gameSettings->setNetworkPlayerName(slotIndex, GameConstants::NETWORK_SLOT_UNCONNECTED_SLOTNAME);
					setPlayerNameText(i,"");
				}
			}
			else if (getSelectedPlayerControlTypeIndex(i) != ctHuman) {
				AIPlayerCount++;
				if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d] i = %d, playername is AI (blank)\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,i);

				Lang &lang= Lang::getInstance();
				gameSettings->setNetworkPlayerName(slotIndex, lang.getString("AI") + intToStr(AIPlayerCount));
				setPlayerNameText(i,"");
			}
			if (getSelectedPlayerControlTypeIndex(i) == ctHuman) {
				setSlotHuman(i);
			}
			if(serverInterface != NULL && serverInterface->getSlot(i,true) != NULL) {
				gameSettings->setNetworkPlayerUUID(slotIndex,serverInterface->getSlot(i,true)->getUUID());
				gameSettings->setNetworkPlayerPlatform(slotIndex,serverInterface->getSlot(i,true)->getPlatform());
			}

			factionCount++;
		}
		else {
			//gameSettings->setNetworkPlayerName("");
			gameSettings->setNetworkPlayerStatuses(factionCount, npst_None);
			setPlayerNameText(i,"");
		}
    }

	// Next save closed slots
	int closedCount = 0;
	for(int i = 0; i < GameConstants::maxPlayers; ++i) {
		ControlType ct= static_cast<ControlType>(getSelectedPlayerControlTypeIndex(i));
		if(ct == ctClosed) {
			int slotIndex = factionCount + closedCount;

			gameSettings->setFactionControl(slotIndex, ct);
			gameSettings->setTeam(slotIndex, getSelectedPlayerTeamIndex(i));
			gameSettings->setStartLocationIndex(slotIndex, i);
			//gameSettings->setResourceMultiplierIndex(slotIndex, 10);
			//listBoxRMultiplier[i].setSelectedItem("1.0");
			cegui_manager.setSpinnerControlValue(
					cegui_manager.getControl(
							"SpinnerPlayer" + intToStr(i+1) + "Multiplier"),1.0);

			//printf("FILE: [%s: %d] value: %f\n",__FILE__,__LINE__,
			//		cegui_manager.getSpinnerControlValue(cegui_manager.getControl(
			//				"SpinnerPlayer" + intToStr(i+1) + "Multiplier")));

			//gameSettings->setResourceMultiplierIndex(slotIndex, listBoxRMultiplier[i].getSelectedItemIndex());
			gameSettings->setResourceMultiplierIndex(slotIndex,
					convertMultiplierValueToIndex(cegui_manager.getSpinnerControlValue(
							cegui_manager.getControl(
									"SpinnerPlayer" + intToStr(i+1) + "Multiplier"))));

			//printf("Test multiplier = %s\n",listBoxRMultiplier[i].getSelectedItem().c_str());

			//if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d] i = %d, factionFiles[listBoxFactions[i].getSelectedItemIndex()] [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,i,factionFiles[listBoxFactions[i].getSelectedItemIndex()].c_str());

			//gameSettings->setFactionTypeName(slotIndex, factionFiles[listBoxFactions[i].getSelectedItemIndex()]);
			gameSettings->setFactionTypeName(slotIndex, factionFiles.at(getSelectedPlayerFactionTypeIndex(i)));

			gameSettings->setNetworkPlayerName(slotIndex, "Closed");
			gameSettings->setNetworkPlayerUUID(slotIndex,"");
			gameSettings->setNetworkPlayerPlatform(slotIndex,"");

			closedCount++;
		}
    }

	gameSettings->setFactionCount(factionCount);

	Config &config = Config::getInstance();
	gameSettings->setEnableServerControlledAI(config.getBool("ServerControlledAI","true"));
	gameSettings->setNetworkFramePeriod(config.getInt("NetworkSendFrameCount","20"));
	gameSettings->setNetworkPauseGameForLaggedClients(((getNetworkPauseGameForLaggedClients() == true)));

	if( gameSettings->getTileset() != "") {
		// Check if client has different data, if so force a CRC refresh
		bool forceRefresh = false;
		if(checkNetworkPlayerDataSynch(false,true, false) == false &&
				last_Forced_CheckedCRCTilesetName != gameSettings->getTileset()) {
			lastCheckedCRCTilesetName = "";
			forceRefresh = true;
			last_Forced_CheckedCRCTilesetName = gameSettings->getTileset();
		}

		if(lastCheckedCRCTilesetName != gameSettings->getTileset()) {
			//console.addLine("Checking tileset CRC [" + gameSettings->getTileset() + "]");
			lastCheckedCRCTilesetValue = getFolderTreeContentsCheckSumRecursively(config.getPathListForType(ptTilesets,""), string("/") + gameSettings->getTileset() + string("/*"), ".xml", NULL,forceRefresh);
			if(lastCheckedCRCTilesetValue == 0) {
				lastCheckedCRCTilesetValue = getFolderTreeContentsCheckSumRecursively(config.getPathListForType(ptTilesets,""), string("/") + gameSettings->getTileset() + string("/*"), ".xml", NULL, true);
			}
			lastCheckedCRCTilesetName = gameSettings->getTileset();
		}
		gameSettings->setTilesetCRC(lastCheckedCRCTilesetValue);
	}

	if(config.getBool("DisableServerLobbyTechtreeCRCCheck","false") == false) {
		if(gameSettings->getTech() != "") {
			// Check if client has different data, if so force a CRC refresh
			bool forceRefresh = false;
			if(checkNetworkPlayerDataSynch(false,false,true) == false &&
					last_Forced_CheckedCRCTechtreeName != gameSettings->getTech()) {
				lastCheckedCRCTechtreeName = "";
				forceRefresh = true;
				last_Forced_CheckedCRCTechtreeName = gameSettings->getTech();
			}

			if(lastCheckedCRCTechtreeName != gameSettings->getTech()) {
				//console.addLine("Checking techtree CRC [" + gameSettings->getTech() + "]");
				lastCheckedCRCTechtreeValue = getFolderTreeContentsCheckSumRecursively(config.getPathListForType(ptTechs,""), "/" + gameSettings->getTech() + "/*", ".xml", NULL,forceRefresh);
				if(lastCheckedCRCTechtreeValue == 0) {
					lastCheckedCRCTechtreeValue = getFolderTreeContentsCheckSumRecursively(config.getPathListForType(ptTechs,""), "/" + gameSettings->getTech() + "/*", ".xml", NULL, true);
				}

				reloadFactions(true,(getAllowNetworkScenario() == true ? scenarioFiles.at(getSelectedNetworkScenarioIndex()) : ""));
				factionCRCList.clear();
				for(unsigned int factionIdx = 0; factionIdx < factionFiles.size(); ++factionIdx) {
					string factionName = factionFiles.at(factionIdx);
					if(factionName != GameConstants::RANDOMFACTION_SLOTNAME &&
						factionName != GameConstants::OBSERVER_SLOTNAME) {
						//factionCRC   = getFolderTreeContentsCheckSumRecursively(config.getPathListForType(ptTechs,""), "/" + gameSettings->getTech() + "/factions/" + factionName + "/*", ".xml", NULL, true);
						uint32 factionCRC   = getFolderTreeContentsCheckSumRecursively(config.getPathListForType(ptTechs,""), "/" + gameSettings->getTech() + "/factions/" + factionName + "/*", ".xml", NULL);
						if(factionCRC == 0) {
							factionCRC   = getFolderTreeContentsCheckSumRecursively(config.getPathListForType(ptTechs,""), "/" + gameSettings->getTech() + "/factions/" + factionName + "/*", ".xml", NULL, true);
						}
						factionCRCList.push_back(make_pair(factionName,factionCRC));
					}
				}
				//console.addLine("Found factions: " + intToStr(factionCRCList.size()));
				lastCheckedCRCTechtreeName = gameSettings->getTech();
			}

			gameSettings->setFactionCRCList(factionCRCList);
			gameSettings->setTechCRC(lastCheckedCRCTechtreeValue);
		}
	}

	if(gameSettings->getMap() != "") {
		// Check if client has different data, if so force a CRC refresh
		//bool forceRefresh = false;
		if(checkNetworkPlayerDataSynch(true,false,false) == false &&
				last_Forced_CheckedCRCMapName != gameSettings->getMap()) {
			lastCheckedCRCMapName = "";
			//forceRefresh = true;
			last_Forced_CheckedCRCMapName = gameSettings->getMap();
		}

		if(lastCheckedCRCMapName != gameSettings->getMap()) {
			Checksum checksum;
			string file = Config::getMapPath(gameSettings->getMap(),"",false);
			//console.addLine("Checking map CRC [" + file + "]");
			checksum.addFile(file);
			lastCheckedCRCMapValue = checksum.getSum();
			lastCheckedCRCMapName = gameSettings->getMap();
		}
		gameSettings->setMapCRC(lastCheckedCRCMapValue);
	}

	if(this->headlessServerMode == true) {
		time_t clientConnectedTime = 0;
		bool masterserver_admin_found=false;
		//printf("mapInfo.players [%d]\n",mapInfo.players);

		for(int i= 0; i < mapInfo.players; ++i) {
			if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

			if(getSelectedPlayerControlTypeIndex(i) == ctNetwork ||
					getSelectedPlayerControlTypeIndex(i) == ctNetworkUnassigned) {

				if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

				if(	serverInterface->getSlot(i,true) != NULL && serverInterface->getSlot(i,true)->isConnected()) {
					if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

					//printf("slot = %d serverInterface->getSlot(i)->getConnectedTime() = %d session key [%d]\n",i,serverInterface->getSlot(i)->getConnectedTime(),serverInterface->getSlot(i)->getSessionKey());

					if(clientConnectedTime == 0 ||
							(serverInterface->getSlot(i,true)->getConnectedTime() > 0 && serverInterface->getSlot(i,true)->getConnectedTime() < clientConnectedTime)) {
						clientConnectedTime = serverInterface->getSlot(i,true)->getConnectedTime();
						gameSettings->setMasterserver_admin(serverInterface->getSlot(i,true)->getSessionKey());
						gameSettings->setMasterserver_admin_faction_index(serverInterface->getSlot(i,true)->getPlayerIndex());
						//labelGameName.setText(serverInterface->getSlot(i,true)->getName()+" controls");
						MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
						cegui_manager.setControlText("EditboxPublishGameName",serverInterface->getSlot(i,true)->getName()+" controls");

						//printf("slot = %d, admin key [%d] slot connected time[" MG_SIZE_T_SPECIFIER "] clientConnectedTime [" MG_SIZE_T_SPECIFIER "]\n",i,gameSettings->getMasterserver_admin(),serverInterface->getSlot(i)->getConnectedTime(),clientConnectedTime);
					}
					if(serverInterface->getSlot(i,true)->getSessionKey() == gameSettings->getMasterserver_admin()){
						masterserver_admin_found=true;
					}
				}
			}
		}
		if(masterserver_admin_found == false ) {
			for(int i=mapInfo.players; i < GameConstants::maxPlayers; ++i) {
				ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
				//ConnectionSlot *slot = serverInterface->getSlot(i);

				if(	serverInterface->getSlot(i,true) != NULL && serverInterface->getSlot(i,true)->isConnected()) {
					if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

					//printf("slot = %d serverInterface->getSlot(i)->getConnectedTime() = %d session key [%d]\n",i,serverInterface->getSlot(i)->getConnectedTime(),serverInterface->getSlot(i)->getSessionKey());

					if(clientConnectedTime == 0 ||
							(serverInterface->getSlot(i,true)->getConnectedTime() > 0 && serverInterface->getSlot(i,true)->getConnectedTime() < clientConnectedTime)) {
						clientConnectedTime = serverInterface->getSlot(i,true)->getConnectedTime();
						gameSettings->setMasterserver_admin(serverInterface->getSlot(i,true)->getSessionKey());
						gameSettings->setMasterserver_admin_faction_index(serverInterface->getSlot(i,true)->getPlayerIndex());
						//labelGameName.setText(serverInterface->getSlot(i,true)->getName()+" controls");
						MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
						cegui_manager.setControlText("EditboxPublishGameName",serverInterface->getSlot(i,true)->getName()+" controls");

						//printf("slot = %d, admin key [%d] slot connected time[" MG_SIZE_T_SPECIFIER "] clientConnectedTime [" MG_SIZE_T_SPECIFIER "]\n",i,gameSettings->getMasterserver_admin(),serverInterface->getSlot(i)->getConnectedTime(),clientConnectedTime);
					}
					if(serverInterface->getSlot(i,true)->getSessionKey() == gameSettings->getMasterserver_admin()){
						masterserver_admin_found=true;
					}
				}
			}
		}

		if(masterserver_admin_found == false) {
			//labelGameName.setText("Headless: "+defaultPlayerName);
			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			cegui_manager.setControlText("EditboxPublishGameName","Headless: "+defaultPlayerName);
		}
	}

	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s] Line: %d\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
}

void MenuStateCustomGame::saveGameSettingsToFile(std::string fileName) {
	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s] Line: %d\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	GameSettings gameSettings;
	loadGameSettings(&gameSettings);
	CoreData::getInstance().saveGameSettingsToFile(fileName, &gameSettings,getShowAdvancedOptions());

	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s] Line: %d\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
}

void MenuStateCustomGame::KeepCurrentHumanPlayerSlots(GameSettings &gameSettings) {
	//look for human players
	bool foundValidHumanControlTypeInFile = false;
	for(int index2 = 0; index2 < GameConstants::maxPlayers; ++index2) {
		ControlType ctFile = static_cast<ControlType>(gameSettings.getFactionControl(index2));
		if(ctFile == ctHuman) {
			ControlType ctUI = static_cast<ControlType>(getSelectedPlayerControlTypeIndex(index2));
			if(ctUI != ctNetwork && ctUI != ctNetworkUnassigned) {
				foundValidHumanControlTypeInFile = true;
				//printf("Human found in file [%d]\n",index2);
			}
			else if(getPlayerNameText(index2) == "") {
				foundValidHumanControlTypeInFile = true;
			}
		}
	}

	for(int index = 0; index < GameConstants::maxPlayers; ++index) {
		ControlType ct= static_cast<ControlType>(getSelectedPlayerControlTypeIndex(index));
		if(ct == ctHuman) {
			//printf("Human found in UI [%d] and file [%d]\n",index,foundControlType);

			if(foundValidHumanControlTypeInFile == false) {
				gameSettings.setFactionControl(index,ctHuman);
				gameSettings.setNetworkPlayerName(index,getHumanPlayerName());
			}
		}

		ControlType ctFile = static_cast<ControlType>(gameSettings.getFactionControl(index));
		if(ctFile == ctHuman) {
			gameSettings.setFactionControl(index,ctHuman);
		}
	}
}

GameSettings MenuStateCustomGame::loadGameSettingsFromFile(std::string fileName) {
	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s] Line: %d\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

    GameSettings gameSettings;

    GameSettings originalGameSettings;
	loadGameSettings(&originalGameSettings);

    try {
    	CoreData::getInstance().loadGameSettingsFromFile(fileName, &gameSettings);
    	KeepCurrentHumanPlayerSlots(gameSettings);

    	// correct game settings for headless:
    	if(this->headlessServerMode == true) {
    		for(int i = 0; i < GameConstants::maxPlayers; ++i) {
    			if(gameSettings.getFactionControl(i)== ctHuman){
    				gameSettings.setFactionControl(i,ctNetwork);
    			}
    		}
    	}
		setupUIFromGameSettings(gameSettings);
	}
    catch(const exception &ex) {
    	SystemFlags::OutputDebug(SystemFlags::debugError,"In [%s::%s Line: %d] Error [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,ex.what());
    	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d] ERROR = [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,ex.what());

    	showMessageBox( ex.what(), "Error", false);

    	setupUIFromGameSettings(originalGameSettings);
    	gameSettings = originalGameSettings;
    }

    if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s] Line: %d\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	return gameSettings;
}

void MenuStateCustomGame::setupUIFromGameSettings(const GameSettings &gameSettings) {
	string humanPlayerName = getHumanPlayerName();

	string scenarioDir = "";
	setAllowNetworkScenario((gameSettings.getScenario() != ""));
	if(getAllowNetworkScenario() == true) {
		setSelectedNetworkScenario(formatString(gameSettings.getScenario()));

		loadScenarioInfo(Scenario::getScenarioPath(dirList, scenarioFiles.at(getSelectedNetworkScenarioIndex())), &scenarioInfo);
		scenarioDir = Scenario::getScenarioDir(dirList, gameSettings.getScenario());

		//printf("scenarioInfo.fogOfWar = %d scenarioInfo.fogOfWar_exploredFlag = %d\n",scenarioInfo.fogOfWar,scenarioInfo.fogOfWar_exploredFlag);

		MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();

		if(scenarioInfo.fogOfWar == false && scenarioInfo.fogOfWar_exploredFlag == false) {
			//listBoxFogOfWar.setSelectedItemIndex(2);
			cegui_manager.setSelectedItemInComboBoxControl(cegui_manager.getControl("ComboBoxFogOfWar"),2);
		}
		else if(scenarioInfo.fogOfWar_exploredFlag == true) {
			//listBoxFogOfWar.setSelectedItemIndex(1);
			cegui_manager.setSelectedItemInComboBoxControl(cegui_manager.getControl("ComboBoxFogOfWar"),1);
		}
		else {
			//listBoxFogOfWar.setSelectedItemIndex(0);
			cegui_manager.setSelectedItemInComboBoxControl(cegui_manager.getControl("ComboBoxFogOfWar"),0);
		}
	}
	setupMapList(gameSettings.getScenario());
	setupTechList(gameSettings.getScenario(),false);
	setupTilesetList(gameSettings.getScenario());

	if(getAllowNetworkScenario() == true) {
		//string file = Scenario::getScenarioPath(dirList, gameSettings.getScenario());
		//loadScenarioInfo(file, &scenarioInfo);

		//printf("#6.1 about to load map [%s]\n",scenarioInfo.mapName.c_str());
		//loadMapInfo(Config::getMapPath(scenarioInfo.mapName, scenarioDir, true), &mapInfo, false);
		//printf("#6.2\n");

		//listBoxMapFilter.setSelectedItemIndex(0);
		MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
		cegui_manager.setSelectedItemInComboBoxControl(cegui_manager.getControl("ComboMapFilter"),0);

		//listBoxMap.setItems(formattedPlayerSortedMaps[mapInfo.players]);
		//listBoxMap.setSelectedItem(formatString(scenarioInfo.mapName));

		cegui_manager.addItemsToComboBoxControl(
					cegui_manager.getControl("ComboMap"),formattedPlayerSortedMaps[mapInfo.players]);
		cegui_manager.setSelectedItemInComboBoxControl(
				cegui_manager.getControl("ComboMap"),formatString(scenarioInfo.mapName));
	}
	else {
		MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
		int filterCount = cegui_manager.getItemCountInComboBoxControl(cegui_manager.getControl("ComboMapFilter"));

		if(filterCount > 0) {
			if(gameSettings.getMapFilterIndex() == 0) {
				cegui_manager.setSelectedItemInComboBoxControl(cegui_manager.getControl("ComboMapFilter"),0);
			}
			else {
				//listBoxMapFilter.setSelectedItem(intToStr(gameSettings.getMapFilterIndex()));
				printf("gameSettings.getMapFilterIndex(): %d\n",gameSettings.getMapFilterIndex());
				cegui_manager.setSelectedItemInComboBoxControl(cegui_manager.getControl("ComboMapFilter"),gameSettings.getMapFilterIndex());
			}
		}
		//listBoxMap.setItems(formattedPlayerSortedMaps[gameSettings.getMapFilterIndex()]);
		cegui_manager.addItemsToComboBoxControl(
					cegui_manager.getControl("ComboMap"),formattedPlayerSortedMaps[gameSettings.getMapFilterIndex()]);
	}

	//printf("gameSettings.getMap() [%s] [%s]\n",gameSettings.getMap().c_str(),listBoxMap.getSelectedItem().c_str());

	string mapFile = gameSettings.getMap();
	if(find(mapFiles.begin(),mapFiles.end(),mapFile) != mapFiles.end()) {
		mapFile = formatString(mapFile);
		//listBoxMap.setSelectedItem(mapFile);
		MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
		cegui_manager.setSelectedItemInComboBoxControl(
				cegui_manager.getControl("ComboMap"),mapFile);

		loadMapInfo(Config::getMapPath(getCurrentMapFile(),scenarioDir,true), &mapInfo, true);
		//labelMapInfo.setText(mapInfo.desc);

		cegui_manager.setControlText("LabelMapInfo",mapInfo.desc);

	}

	string tilesetFile = gameSettings.getTileset();
	if(find(tilesetFiles.begin(),tilesetFiles.end(),tilesetFile) != tilesetFiles.end()) {
		tilesetFile = formatString(tilesetFile);
		//listBoxTileset.setSelectedItem(tilesetFile);

		MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
		cegui_manager.setSelectedItemInComboBoxControl(cegui_manager.getControl("ComboBoxTileset"),tilesetFile);
	}

	string techtreeFile = gameSettings.getTech();
	if(find(techTreeFiles.begin(),techTreeFiles.end(),techtreeFile) != techTreeFiles.end()) {
		techtreeFile = formatString(techtreeFile);

		MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();

		//listBoxTechTree.setSelectedItem(techtreeFile);
		cegui_manager.setSelectedItemInComboBoxControl(cegui_manager.getControl("ComboBoxTechTree"),techtreeFile);
	}

	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s] Line: %d\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	//gameSettings->setDefaultUnits(true);
	//gameSettings->setDefaultResources(true);
	//gameSettings->setDefaultVictoryConditions(true);

	//FogOfWar
	if(getAllowNetworkScenario() == false) {

		MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();

		//listBoxFogOfWar.setSelectedItemIndex(0); // default is 0!
		cegui_manager.setSelectedItemInComboBoxControl(cegui_manager.getControl("ComboBoxFogOfWar"),0);
		if(gameSettings.getFogOfWar() == false){
			//listBoxFogOfWar.setSelectedItemIndex(2);
			cegui_manager.setSelectedItemInComboBoxControl(cegui_manager.getControl("ComboBoxFogOfWar"),2);
		}

		if((gameSettings.getFlagTypes1() & ft1_show_map_resources) == ft1_show_map_resources){
			if(gameSettings.getFogOfWar() == true){
				//listBoxFogOfWar.setSelectedItemIndex(1);
				cegui_manager.setSelectedItemInComboBoxControl(cegui_manager.getControl("ComboBoxFogOfWar"),1);
			}
		}
	}

	//printf("In [%s::%s line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	setAllowObservers(gameSettings.getAllowObservers() == true ? true : false);
	//listBoxEnableObserverMode.setSelectedItem(gameSettings.getEnableObserverModeAtEndGame() == true ? lang.getString("Yes") : lang.getString("No"));

	setAllowSwitchTeams((gameSettings.getFlagTypes1() & ft1_allow_team_switching) == ft1_allow_team_switching ? true : false);
	//listBoxAISwitchTeamAcceptPercent.setSelectedItem(intToStr(gameSettings.getAiAcceptSwitchTeamPercentChance()));
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl(
					"ComboAIAcceptPercent");
	cegui_manager.setSelectedItemInComboBoxControl(ctl,intToStr(gameSettings.getAiAcceptSwitchTeamPercentChance()));

	//listBoxFallbackCpuMultiplier.setSelectedItemIndex(gameSettings.getFallbackCpuMultiplier());
	cegui_manager.setSpinnerControlValue(cegui_manager.getControl("SpinnerAIReplaceMultiplier"),gameSettings.getFallbackCpuMultiplier());

	setAllowInGameJoinPlayer((gameSettings.getFlagTypes1() & ft1_allow_in_game_joining) == ft1_allow_in_game_joining ? true : false);

	setAllowTeamUnitSharing((gameSettings.getFlagTypes1() & ft1_allow_shared_team_units) == ft1_allow_shared_team_units ? true : false);
	setAllowTeamResourceSharing((gameSettings.getFlagTypes1() & ft1_allow_shared_team_resources) == ft1_allow_shared_team_resources ? true : false);

	ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
	if(serverInterface != NULL) {
		serverInterface->setAllowInGameConnections(getAllowInGameJoinPlayer() == true);
	}

	setAllowNativeLanguageTechtree(gameSettings.getNetworkAllowNativeLanguageTechtree());

	//listBoxPathFinderType.setSelectedItemIndex(gameSettings.getPathFinderType());

	//listBoxEnableServerControlledAI.setSelectedItem(gameSettings.getEnableServerControlledAI() == true ? lang.getString("Yes") : lang.getString("No"));

	//labelNetworkFramePeriod.setText(lang.getString("NetworkFramePeriod"));

	//listBoxNetworkFramePeriod.setSelectedItem(intToStr(gameSettings.getNetworkFramePeriod()/10*10));

	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s] Line: %d\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	setNetworkPauseGameForLaggedClients(gameSettings.getNetworkPauseGameForLaggedClients());

	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s] Line: %d\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	reloadFactions(false,(getAllowNetworkScenario() == true ? scenarioFiles.at(getSelectedNetworkScenarioIndex()) : ""));
	//reloadFactions(true);

	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s] Line: %d] gameSettings.getFactionCount() = %d\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,gameSettings.getFactionCount());

	for(int i = 0; i < GameConstants::maxPlayers; ++i) {
		int slotIndex = gameSettings.getStartLocationIndex(i);
		if(gameSettings.getFactionControl(i) < getSelectedPlayerControlTypeItemCount(slotIndex)) {
			//listBoxControls[slotIndex].setSelectedItemIndex(gameSettings.getFactionControl(i));
			setPlayerControlTypeSelectedIndex(slotIndex,gameSettings.getFactionControl(i));
		}

		//if(slotIndex == 0) printf("#2 slotIndex = %d, i = %d, multiplier = %d\n",slotIndex,i,listBoxRMultiplier[i].getSelectedItemIndex());

		updateResourceMultiplier(slotIndex);

		//if(slotIndex == 0) printf("#3 slotIndex = %d, i = %d, multiplier = %d\n",slotIndex,i,listBoxRMultiplier[i].getSelectedItemIndex());

		//listBoxRMultiplier[slotIndex].setSelectedItemIndex(gameSettings.getResourceMultiplierIndex(i));
		MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
		cegui_manager.setSpinnerControlValue(
				cegui_manager.getControl(
						"SpinnerPlayer" + intToStr(slotIndex+1) + "Multiplier"),
						convertMultiplierIndexToValue(gameSettings.getResourceMultiplierIndex(i)));

//		printf("FILE: [%s: %d] value: %f\n",__FILE__,__LINE__,
//				cegui_manager.getSpinnerControlValue(cegui_manager.getControl(
//						"SpinnerPlayer" + intToStr(slotIndex+1) + "Multiplier")));

		//if(slotIndex == 0) printf("#4 slotIndex = %d, i = %d, multiplier = %d\n",slotIndex,i,listBoxRMultiplier[i].getSelectedItemIndex());

		setSelectedPlayerTeamIndex(slotIndex,gameSettings.getTeam(i));

		lastSelectedTeamIndex[slotIndex] = getSelectedPlayerTeamIndex(slotIndex);

		string factionName = gameSettings.getFactionTypeName(i);
		factionName = formatString(factionName);

		//printf("\n\n\n*** setupUIFromGameSettings A, i = %d, startLoc = %d, factioncontrol = %d, factionName [%s]\n",i,gameSettings.getStartLocationIndex(i),gameSettings.getFactionControl(i),factionName.c_str());

		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d] factionName = [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,factionName.c_str());

		//if(listBoxFactions[slotIndex].hasItem(factionName) == true) {
		//	listBoxFactions[slotIndex].setSelectedItem(factionName);
		//}
		//else {
		//	listBoxFactions[slotIndex].setSelectedItem(formatString(GameConstants::RANDOMFACTION_SLOTNAME));
		//}

		if(hasPlayerFactionTypeItem(slotIndex,factionName) == true) {
			setPlayerFactionTypeSelectedItem(slotIndex,factionName);
		}
		else {
			setPlayerFactionTypeSelectedItem(slotIndex,formatString(GameConstants::RANDOMFACTION_SLOTNAME));
		}
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d] i = %d, gameSettings.getNetworkPlayerName(i) [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,i,gameSettings.getNetworkPlayerName(i).c_str());

		//labelPlayerNames[slotIndex].setText(gameSettings.getNetworkPlayerName(i));
	}

	//SetActivePlayerNameEditor();

	updateControlers();
	updateNetworkSlots();

	if(this->headlessServerMode == false && humanPlayerName != "") {
		for(int index = 0; index < GameConstants::maxPlayers; ++index) {
			ControlType ct= static_cast<ControlType>(getSelectedPlayerControlTypeIndex(index));
			if(ct == ctHuman) {
				if(humanPlayerName != getPlayerNameText(index)) {
					//printf("Player name changing from [%s] to [%s]\n",labelPlayerNames[index].getText().c_str(),humanPlayerName.c_str());

					setPlayerNameText(index,"");
					setPlayerNameText(index,humanPlayerName);
				}
			}
		}
	}

	if(hasNetworkGameSettings() == true) {
		needToSetChangedGameSettings = true;
		lastSetChangedGameSettings   = time(NULL);
	}
}
// ============ PRIVATE ===========================

bool MenuStateCustomGame::hasNetworkGameSettings() {
    bool hasNetworkSlot = false;

    try {
		for(int i=0; i<mapInfo.players; ++i) {
			ControlType ct= static_cast<ControlType>(getSelectedPlayerControlTypeIndex(i));
			if(ct != ctClosed) {
				if(ct == ctNetwork || ct == ctNetworkUnassigned) {
					hasNetworkSlot = true;
					break;
				}
			}
		}
		if(hasNetworkSlot == false) {
			for(int i=0; i < GameConstants::maxPlayers; ++i) {
				ControlType ct= static_cast<ControlType>(getSelectedPlayerControlTypeIndex(i));
				if(ct != ctClosed) {
					if(ct == ctNetworkUnassigned) {
						hasNetworkSlot = true;
						break;
					}
				}
			}
		}
    }
	catch(const std::exception &ex) {
		char szBuf[8096]="";
		snprintf(szBuf,8096,"In [%s::%s %d]\nError detected:\n%s\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,ex.what());
		SystemFlags::OutputDebug(SystemFlags::debugError,"In [%s::%s Line: %d] Error [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,ex.what());
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s",szBuf);

		showGeneralError=true;
		generalErrorToShow = szBuf;
	}

    return hasNetworkSlot;
}

void MenuStateCustomGame::loadMapInfo(string file, MapInfo *mapInfo, bool loadMapPreview) {
	try {

		Lang &lang= Lang::getInstance();
		if(MapPreview::loadMapInfo(file, mapInfo, lang.getString("MaxPlayers"),lang.getString("Size"),true) == true) {

			if(getSelectedPlayerControlTypeItemCount(0) > 0) {
				ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
				for(int i = 0; i < GameConstants::maxPlayers; ++i) {
					if(serverInterface->getSlot(i,true) != NULL &&
						(getSelectedPlayerControlTypeIndex(i) == ctNetwork ||
								getSelectedPlayerControlTypeIndex(i) == ctNetworkUnassigned)) {

						if(serverInterface->getSlot(i,true)->isConnected() == true) {
							if(i+1 > mapInfo->players &&
									getSelectedPlayerControlTypeIndex(i) != ctNetworkUnassigned) {
								//listBoxControls[i].setSelectedItemIndex(ctNetworkUnassigned);
								setPlayerControlTypeSelectedIndex(i,ctNetworkUnassigned);
							}
						}
					}

					//labelPlayers[i].setVisible(i+1 <= mapInfo->players);
					setPlayerNumberVisible(i, i+1 <= mapInfo->players);

					setPlayerNameVisible(i,i+1 <= mapInfo->players);
					//listBoxControls[i].setVisible(i+1 <= mapInfo->players);
					setPlayerControlTypeVisible(i,i+1 <= mapInfo->players);
					//listBoxFactions[i].setVisible(i+1 <= mapInfo->players);

					MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
					cegui_manager.setControlVisible(
					    cegui_manager.getControl(
						    "ComboBoxPlayer" + intToStr(i+1) + "Faction"),
						    i+1 <= mapInfo->players);

					setPlayerTeamVisible(i,i+1 <= mapInfo->players);
					//labelNetStatus[i].setVisible(i+1 <= mapInfo->players);
					setPlayerVersionVisible(i,i+1 <= mapInfo->players);
				}
			}
			// Not painting properly so this is on hold
			if(loadMapPreview == true) {
				if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

				mapPreview.loadFromFile(file.c_str());

				//printf("Loading map preview MAP\n");
				cleanupMapPreviewTexture();
			}
		}

	}
	catch(exception &e) {
		SystemFlags::OutputDebug(SystemFlags::debugError,"In [%s::%s Line: %d] Error [%s] loading map [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,e.what(),file.c_str());
		throw megaglest_runtime_error("Error loading map file: [" + file + "] msg: " + e.what());
	}
}

void MenuStateCustomGame::updateControlers() {
	try {
		bool humanPlayer= false;

		for(int i = 0; i < mapInfo.players; ++i) {
			if(getSelectedPlayerControlTypeIndex(i) == ctHuman) {
				humanPlayer= true;
			}
		}

		if(humanPlayer == false) {
			if(this->headlessServerMode == false) {
				bool foundNewSlotForHuman = false;
				for(int i = 0; i < mapInfo.players; ++i) {
					if(getSelectedPlayerControlTypeIndex(i) == ctClosed) {
						setSlotHuman(i);
						foundNewSlotForHuman = true;
						break;
					}
				}

				if(foundNewSlotForHuman == false) {
					for(int i = 0; i < mapInfo.players; ++i) {
						if(getSelectedPlayerControlTypeIndex(i) == ctClosed ||
							getSelectedPlayerControlTypeIndex(i) == ctCpuEasy ||
							getSelectedPlayerControlTypeIndex(i) == ctCpu ||
							getSelectedPlayerControlTypeIndex(i) == ctCpuUltra ||
							getSelectedPlayerControlTypeIndex(i) == ctCpuMega) {

							setSlotHuman(i);

							foundNewSlotForHuman = true;
							break;
						}
					}
				}

				if(foundNewSlotForHuman == false) {
					ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
					ConnectionSlot *slot = serverInterface->getSlot(0,true);
					if(slot != NULL && slot->isConnected() == true) {
						serverInterface->removeSlot(0);
					}
					setSlotHuman(0);
				}
			}
		}

		for(int i= mapInfo.players; i < GameConstants::maxPlayers; ++i) {
			if( getSelectedPlayerControlTypeIndex(i) != ctNetwork &&
					getSelectedPlayerControlTypeIndex(i) != ctNetworkUnassigned) {
				//printf("Closed A [%d] [%s]\n",i,labelPlayerNames[i].getText().c_str());

				//listBoxControls[i].setSelectedItemIndex(ctClosed);
				setPlayerControlTypeSelectedIndex(i,ctClosed);
			}
		}
	}
	catch(const std::exception &ex) {
		char szBuf[8096]="";
		snprintf(szBuf,8096,"In [%s::%s %d]\nError detected:\n%s\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,ex.what());
		SystemFlags::OutputDebug(SystemFlags::debugError,szBuf);
		throw megaglest_runtime_error(szBuf);
	}
}

void MenuStateCustomGame::closeUnusedSlots(){
	try {
		if(getAllowNetworkScenario() == false) {
			ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
			//for(int i= 0; i<mapInfo.players; ++i){
			for(int i= 0; i < GameConstants::maxPlayers; ++i){
				if(getSelectedPlayerControlTypeIndex(i) == ctNetwork ||
						getSelectedPlayerControlTypeIndex(i) == ctNetworkUnassigned) {

					if(serverInterface->getSlot(i,true) == NULL ||
					   serverInterface->getSlot(i,true)->isConnected() == false ||
					   serverInterface->getSlot(i,true)->getConnectHasHandshaked() == false) {
						//printf("Closed A [%d] [%s]\n",i,labelPlayerNames[i].getText().c_str());

						//listBoxControls[i].setSelectedItemIndex(ctClosed);
						setPlayerControlTypeSelectedIndex(i,ctClosed);
					}
				}
			}
			updateNetworkSlots();
		}
	}
	catch(const std::exception &ex) {
		char szBuf[8096]="";
		snprintf(szBuf,8096,"In [%s::%s %d]\nError detected:\n%s\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,ex.what());
		SystemFlags::OutputDebug(SystemFlags::debugError,szBuf);
		throw megaglest_runtime_error(szBuf);
	}
}

void MenuStateCustomGame::updateNetworkSlots() {
	try {
		ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();

        if(hasNetworkGameSettings() == true) {
            if(hasCheckedForUPNP == false) {

        		MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
        		bool publishToServerEnabled =
        				cegui_manager.getCheckboxControlChecked(
        						cegui_manager.getControl("CheckboxPublishGame"));

            	if(publishToServerEnabled || this->headlessServerMode) {

            		hasCheckedForUPNP = true;
            		serverInterface->getServerSocket()->NETdiscoverUPnPDevices();
            	}
            }
        }
        else {
            hasCheckedForUPNP = false;
        }

		for(int i= 0; i < GameConstants::maxPlayers; ++i) {
			ConnectionSlot *slot = serverInterface->getSlot(i,true);
			//printf("A i = %d control type = %d slot [%p]\n",i,listBoxControls[i].getSelectedItemIndex(),slot);

			if(slot == NULL &&
				getSelectedPlayerControlTypeIndex(i) == ctNetwork)	{

				try {
					serverInterface->addSlot(i);
				}
				catch(const std::exception &ex) {
					char szBuf[8096]="";
					snprintf(szBuf,8096,"In [%s::%s %d]\nError detected:\n%s\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,ex.what());
					SystemFlags::OutputDebug(SystemFlags::debugError,szBuf);
					if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s",szBuf);
					showGeneralError=true;
					if(serverInterface->isPortBound() == false) {
						generalErrorToShow = Lang::getInstance().getString("ErrorBindingPort") + " : " + intToStr(serverInterface->getBindPort());
					}
					else {
						generalErrorToShow = ex.what();
					}

					// Revert network to CPU
					//listBoxControls[i].setSelectedItemIndex(ctCpu);
					setPlayerControlTypeSelectedIndex(i,ctCpu);
				}
			}
			slot = serverInterface->getSlot(i,true);
			if(slot != NULL) {
				if((getSelectedPlayerControlTypeIndex(i) != ctNetwork) ||
					(getSelectedPlayerControlTypeIndex(i) == ctNetwork &&
						slot->isConnected() == false && i >= mapInfo.players)) {

					if(slot->getCanAcceptConnections() == true) {
						slot->setCanAcceptConnections(false);
					}
					if(slot->isConnected() == true) {
						if(getSelectedPlayerControlTypeIndex(i) != ctNetworkUnassigned) {

							//listBoxControls[i].setSelectedItemIndex(ctNetworkUnassigned);
							setPlayerControlTypeSelectedIndex(i,ctNetworkUnassigned);
						}
					}
					else {
						serverInterface->removeSlot(i);

						if(getSelectedPlayerControlTypeIndex(i) == ctNetworkUnassigned) {

							//listBoxControls[i].setSelectedItemIndex(ctClosed);
							setPlayerControlTypeSelectedIndex(i,ctClosed);
						}
					}
				}
				else if(slot->getCanAcceptConnections() == false) {
					slot->setCanAcceptConnections(true);
				}
			}
		}
	}
	catch(const std::exception &ex) {
		char szBuf[8096]="";
		snprintf(szBuf,8096,"In [%s::%s %d]\nError detected:\n%s\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,ex.what());
		SystemFlags::OutputDebug(SystemFlags::debugError,szBuf);
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s",szBuf);
		//throw megaglest_runtime_error(szBuf);
		showGeneralError=true;
		generalErrorToShow = szBuf;

	}
}

void MenuStateCustomGame::keyDown(SDL_KeyboardEvent key) {
	if(isMasterserverMode() == true) {
		return;
	}

//	if(activeInputLabel != NULL) {
//		bool handled = keyDownEditLabel(key, &activeInputLabel);
//		if(handled == true) {
//			MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//			MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//
//	        if(hasNetworkGameSettings() == true) {
//	            needToSetChangedGameSettings = true;
//	            lastSetChangedGameSettings   = time(NULL);
//	        }
//		}
//	}
	//else {
		//send key to the chat manager
		if(hasNetworkGameSettings() == true) {
			chatManager.keyDown(key);
		}
		if(chatManager.getEditEnabled() == false &&
				(::Shared::Platform::Window::isKeyStateModPressed(KMOD_SHIFT) == false)	) {
			Config &configKeys = Config::getInstance(std::pair<ConfigType,ConfigType>(cfgMainKeys,cfgUserKeys));

			//if(key == configKeys.getCharKey("ShowFullConsole")) {
			if(isKeyPressed(configKeys.getSDLKey("ShowFullConsole"),key) == true) {
				showFullConsole= true;
			}
			//Toggle music
			//else if(key == configKeys.getCharKey("ToggleMusic")) {
			else if(isKeyPressed(configKeys.getSDLKey("ToggleMusic"),key) == true) {
				Config &config = Config::getInstance();
				Lang &lang= Lang::getInstance();

				float configVolume = (config.getInt("SoundVolumeMusic") / 100.f);
				float currentVolume = CoreData::getInstance().getMenuMusic()->getVolume();
				if(currentVolume > 0) {
					CoreData::getInstance().getMenuMusic()->setVolume(0.f);
					console.addLine(lang.getString("GameMusic") + " " + lang.getString("Off"));
				}
				else {
					CoreData::getInstance().getMenuMusic()->setVolume(configVolume);
					//If the config says zero, use the default music volume
					//gameMusic->setVolume(configVolume ? configVolume : 0.9);
					console.addLine(lang.getString("GameMusic"));
				}
			}
			//else if(key == configKeys.getCharKey("SaveGUILayout")) {
			else if(isKeyPressed(configKeys.getSDLKey("SaveGUILayout"),key) == true) {
				bool saved = GraphicComponent::saveAllCustomProperties(containerName);
				Lang &lang= Lang::getInstance();
				console.addLine(lang.getString("GUILayoutSaved") + " [" + (saved ? lang.getString("Yes") : lang.getString("No"))+ "]");
			}
		}
	//}
}

void MenuStateCustomGame::keyPress(SDL_KeyboardEvent c) {
	if(isMasterserverMode() == true) {
		return;
	}

//	if(activeInputLabel != NULL) {
//		bool handled = keyPressEditLabel(c, &activeInputLabel);
//		//if(handled == true && &labelGameName != activeInputLabel) {
//		if(handled == true) {
//			MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//			MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
//
//			if(hasNetworkGameSettings() == true) {
//				needToSetChangedGameSettings = true;
//				lastSetChangedGameSettings   = time(NULL);
//			}
//		}
//	}
//	else {
		if(hasNetworkGameSettings() == true) {
			chatManager.keyPress(c);
		}
//	}
}

void MenuStateCustomGame::keyUp(SDL_KeyboardEvent key) {
	if(isMasterserverMode() == true) {
		return;
	}

	//if(activeInputLabel==NULL) {
		if(hasNetworkGameSettings() == true) {
			chatManager.keyUp(key);
		}
		Config &configKeys = Config::getInstance(std::pair<ConfigType,ConfigType>(cfgMainKeys,cfgUserKeys));

		if(chatManager.getEditEnabled()) {
			//send key to the chat manager
			if(hasNetworkGameSettings() == true) {
				chatManager.keyUp(key);
			}
		}
		//else if(key == configKeys.getCharKey("ShowFullConsole")) {
		else if(isKeyPressed(configKeys.getSDLKey("ShowFullConsole"),key) == true) {
			showFullConsole= false;
		}
	//}
}

void MenuStateCustomGame::showMessageBox(const string &text, const string &header, bool toggle){
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	if(cegui_manager.isMessageBoxShowing() == false) {
		MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
		Lang &lang= Lang::getInstance();
		//if(okOnly == true) {
		cegui_manager.displayMessageBox(header, text, lang.getString("Ok","",false,true),"");
		//}
		//else {
		//cegui_manager.displayMessageBox(header, text, lang.getString("Yes","",false,true),lang.getString("No","",false,true));
		//}
	}
	else {
		cegui_manager.hideMessageBox();
	}
}

//void MenuStateCustomGame::switchToNextMapGroup(const int direction){
//	int i=listBoxMapFilter.getSelectedItemIndex();
//	// if there are no maps for the current selection we switch to next selection
//	while(formattedPlayerSortedMaps[i].empty()){
//		i=i+direction;
//		if(i>GameConstants::maxPlayers){
//			i=0;
//		}
//		if(i<0){
//			i=GameConstants::maxPlayers;
//		}
//	}
//	listBoxMapFilter.setSelectedItemIndex(i);
//	listBoxMap.setItems(formattedPlayerSortedMaps[i]);
//}

void MenuStateCustomGame::switchToMapGroup(const int index) {

	const int playersSupported = index;
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();

	//printf("Switch to group: %d\n",index);
	cegui_manager.setSelectedItemInComboBoxControl(
		cegui_manager.getControl("ComboMapFilter"), index);

	//printf("Map Filter Index: %d playersSupported: %d\n",index,playersSupported);

	cegui_manager.addItemsToComboBoxControl(
			cegui_manager.getControl("ComboMap"), formattedPlayerSortedMaps[playersSupported]);
	if(formattedPlayerSortedMaps[playersSupported].empty() == false) {
		cegui_manager.setSelectedItemInComboBoxControl(
			cegui_manager.getControl("ComboMap"), 0);
	}
	else {
		cegui_manager.setControlText(cegui_manager.getControl("ComboMap"),"");
	}
}

string MenuStateCustomGame::getCurrentMapFile(){
	string mapFile = "";

	//int i=listBoxMapFilter.getSelectedItemIndex();
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	int index = cegui_manager.getSelectedItemIndexFromComboBoxControl(cegui_manager.getControl("ComboMapFilter"));
	if(index >= 0) {
		//int mapIndex = listBoxMap.getSelectedItemIndex();
		size_t mapIndex = cegui_manager.getSelectedItemIndexFromComboBoxControl(
				cegui_manager.getControl("ComboMap"));

		//printf("Current map filter: %d mapIndex: %d\n",index,mapIndex);

		if(playerSortedMaps[index].empty() == false && mapIndex < playerSortedMaps[index].size()) {
			//printf("Line: %d (%d)\n",__LINE__,mapIndex);
			mapFile = playerSortedMaps[index].at(mapIndex);
		}
		else {
			//printf("playerSortedMaps[index].empty() %d playerSortedMaps[index].size(): %d\n",
			//		playerSortedMaps[index].empty(),(int)playerSortedMaps[index].size());
		}
	}
	//printf("index: %d mapFile: [%s]\n",index,mapFile.c_str());
	return mapFile;
}

//void MenuStateCustomGame::setActiveInputLabel(GraphicLabel *newLable) {
//	MenuState::setActiveInputLabel(newLable,&activeInputLabel);
//}

string MenuStateCustomGame::getHumanPlayerName(int index) {
	string  result = defaultPlayerName;
	if(index < 0) {
		for(int j = 0; j < GameConstants::maxPlayers; ++j) {
			if(getSelectedPlayerControlTypeIndex(j) >= 0) {

				ControlType ct = static_cast<ControlType>(getSelectedPlayerControlTypeIndex(j));
				if(ct == ctHuman) {
					index = j;
					break;
				}
			}
		}
	}

	if(index >= 0 && index < GameConstants::maxPlayers &&
			getPlayerNameText(index) != "" &&
			getPlayerNameText(index) !=  GameConstants::NETWORK_SLOT_UNCONNECTED_SLOTNAME) {
		result = getPlayerNameText(index);

//		if(activeInputLabel != NULL) {
//			size_t found = result.find_last_of("_");
//			if (found != string::npos) {
//				result = result.substr(0,found);
//			}
//		}
	}

	return result;
}

void MenuStateCustomGame::loadFactionTexture(string filepath) {
	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	if(enableFactionTexturePreview == true) {
		if(filepath == "") {
			factionTexture = NULL;
		}
		else {
			if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d] filepath = [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,filepath.c_str());

			factionTexture = Renderer::findTexture(filepath);

			if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
		}

		MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
		cegui_manager.setImageFileForControl((factionTexture != NULL ? "ImageFactionPreview_" + filepath : ""),
				(factionTexture != NULL ? factionTexture->getPath() : ""),
					"ImageFactionPreview");

	}
}

void MenuStateCustomGame::cleanupMapPreviewTexture() {
	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

	//printf("CLEANUP map preview texture\n");

	if(mapPreviewTexture != NULL) {
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

		mapPreviewTexture->end();

		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
		delete mapPreviewTexture;
		mapPreviewTexture = NULL;

    	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
    	cegui_manager.setImageForControl("TextureMapPreview",NULL, "ImageMapPreview",false);
	}
	if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line: %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);
}

int32 MenuStateCustomGame::getNetworkPlayerStatus() {
	int32 result = npst_None;

	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	int selectedControlItemIndex = cegui_manager.
			getSelectedItemIndexFromComboBoxControl(
					cegui_manager.getControl("ComboHumanPlayerStatus"));

	switch(selectedControlItemIndex) {
		case 2:
			result = npst_Ready;
			break;
		case 1:
			result = npst_BeRightBack;
			break;
		case 0:
		default:
			result = npst_PickSettings;
			break;
	}

	return result;
}

void MenuStateCustomGame::loadScenarioInfo(string file, ScenarioInfo *scenarioInfo) {
	//printf("Load scenario file [%s]\n",file.c_str());
	bool isTutorial = Scenario::isGameTutorial(file);
	Scenario::loadScenarioInfo(file, scenarioInfo, isTutorial);

	//cleanupPreviewTexture();
	previewLoadDelayTimer=time(NULL);
	needToLoadTextures=true;
}

bool MenuStateCustomGame::isInSpecialKeyCaptureEvent() {
	//bool result = (chatManager.getEditEnabled() || activeInputLabel != NULL);
	bool result = chatManager.getEditEnabled();
	return result;
}

void MenuStateCustomGame::processScenario() {
	try {
		if(getAllowNetworkScenario() == true) {
			//printf("listBoxScenario.getSelectedItemIndex() = %d [%s] scenarioFiles.size() = %d\n",listBoxScenario.getSelectedItemIndex(),listBoxScenario.getSelectedItem().c_str(),scenarioFiles.size());
			loadScenarioInfo(Scenario::getScenarioPath(dirList, scenarioFiles.at(getSelectedNetworkScenarioIndex())), &scenarioInfo);
			string scenarioDir = Scenario::getScenarioDir(dirList, scenarioInfo.name);

			//printf("scenarioInfo.fogOfWar = %d scenarioInfo.fogOfWar_exploredFlag = %d\n",scenarioInfo.fogOfWar,scenarioInfo.fogOfWar_exploredFlag);

			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();

			if(scenarioInfo.fogOfWar == false && scenarioInfo.fogOfWar_exploredFlag == false) {
				//listBoxFogOfWar.setSelectedItemIndex(2);
				cegui_manager.setSelectedItemInComboBoxControl(cegui_manager.getControl("ComboBoxFogOfWar"),2);
			}
			else if(scenarioInfo.fogOfWar_exploredFlag == true) {
				//listBoxFogOfWar.setSelectedItemIndex(1);
				cegui_manager.setSelectedItemInComboBoxControl(cegui_manager.getControl("ComboBoxFogOfWar"),1);
			}
			else {
				//listBoxFogOfWar.setSelectedItemIndex(0);
				cegui_manager.setSelectedItemInComboBoxControl(cegui_manager.getControl("ComboBoxFogOfWar"),0);
			}

			setupTechList(scenarioInfo.name, false);
			//listBoxTechTree.setSelectedItem(formatString(scenarioInfo.techTreeName));

			cegui_manager.setSelectedItemInComboBoxControl(cegui_manager.getControl("ComboBoxTechTree"),formatString(scenarioInfo.techTreeName));
			reloadFactions(false,scenarioInfo.name);

			setupTilesetList(scenarioInfo.name);
			//listBoxTileset.setSelectedItem(formatString(scenarioInfo.tilesetName));
			cegui_manager.setSelectedItemInComboBoxControl(cegui_manager.getControl("ComboBoxTileset"),formatString(scenarioInfo.tilesetName));

			setupMapList(scenarioInfo.name);
			//listBoxMap.setSelectedItem(formatString(scenarioInfo.mapName));
			cegui_manager.setSelectedItemInComboBoxControl(
					cegui_manager.getControl("ComboMap"),formatString(scenarioInfo.mapName));

			loadMapInfo(Config::getMapPath(getCurrentMapFile(),scenarioDir,true), &mapInfo, true);
			//labelMapInfo.setText(mapInfo.desc);

			//MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			cegui_manager.setControlText("LabelMapInfo",mapInfo.desc);

			//printf("scenarioInfo.name [%s] [%s]\n",scenarioInfo.name.c_str(),listBoxMap.getSelectedItem().c_str());

			// Loop twice to set the human slot or else it closes network slots in some cases
			for(int humanIndex = 0; humanIndex < 2; ++humanIndex) {
				for(int i = 0; i < mapInfo.players; ++i) {
					//listBoxRMultiplier[i].setSelectedItem(floatToStr(scenarioInfo.resourceMultipliers[i],1));

					cegui_manager.setSpinnerControlValue(
							cegui_manager.getControl(
									"SpinnerPlayer" + intToStr(i+1) + "Multiplier"),
									scenarioInfo.resourceMultipliers[i]);

//					printf("FILE: [%s: %d] value: %f\n",__FILE__,__LINE__,
//							cegui_manager.getSpinnerControlValue(cegui_manager.getControl(
//									"SpinnerPlayer" + intToStr(i+1) + "Multiplier")));

					ServerInterface* serverInterface= NetworkManager::getInstance().getServerInterface();
					ConnectionSlot *slot = serverInterface->getSlot(i,true);

					int selectedControlItemIndex = getSelectedPlayerControlTypeIndex(i);
					if(selectedControlItemIndex != ctNetwork ||
						(selectedControlItemIndex == ctNetwork && (slot == NULL || slot->isConnected() == false))) {
					}

					//listBoxControls[i].setSelectedItemIndex(scenarioInfo.factionControls[i]);
					setPlayerControlTypeSelectedIndex(i,scenarioInfo.factionControls[i]);

					if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__);

					// Skip over networkunassigned
					//if(listBoxControls[i].getSelectedItemIndex() == ctNetworkUnassigned &&
					//	selectedControlItemIndex != ctNetworkUnassigned) {
					//	listBoxControls[i].mouseClick(x, y);
					//}

					//look for human players
					int humanIndex1= -1;
					int humanIndex2= -1;
					for(int j = 0; j < GameConstants::maxPlayers; ++j) {
						ControlType ct= static_cast<ControlType>(getSelectedPlayerControlTypeIndex(j));
						if(ct == ctHuman) {
							if(humanIndex1 == -1) {
								humanIndex1= j;
							}
							else {
								humanIndex2= j;
							}
						}
					}

					if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d] humanIndex1 = %d, humanIndex2 = %d\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,humanIndex1,humanIndex2);

					//no human
					if(humanIndex1 == -1 && humanIndex2 == -1) {
						setSlotHuman(i);
						if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d] i = %d, labelPlayerNames[i].getText() [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,i,getPlayerNameText(i).c_str());

						//printf("humanIndex1 = %d humanIndex2 = %d i = %d listBoxControls[i].getSelectedItemIndex() = %d\n",humanIndex1,humanIndex2,i,listBoxControls[i].getSelectedItemIndex());
					}
					//2 humans
					else if(humanIndex1 != -1 && humanIndex2 != -1) {
						int closeSlotIndex = (humanIndex1 == i ? humanIndex2: humanIndex1);
						int humanSlotIndex = (closeSlotIndex == humanIndex1 ? humanIndex2 : humanIndex1);

						string origPlayName = getPlayerNameText(closeSlotIndex);

						if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"In [%s::%s Line %d] closeSlotIndex = %d, origPlayName [%s]\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,closeSlotIndex,origPlayName.c_str());
						//printf("humanIndex1 = %d humanIndex2 = %d i = %d closeSlotIndex = %d humanSlotIndex = %d\n",humanIndex1,humanIndex2,i,closeSlotIndex,humanSlotIndex);

						//listBoxControls[closeSlotIndex].setSelectedItemIndex(ctClosed);
						setPlayerControlTypeSelectedIndex(closeSlotIndex,ctClosed);

						setPlayerNameText(humanSlotIndex,(origPlayName != "" ? origPlayName : getHumanPlayerName()));
					}

					ControlType ct= static_cast<ControlType>(getSelectedPlayerControlTypeIndex(i));
					if(ct != ctClosed) {
						//updateNetworkSlots();
						//updateResourceMultiplier(i);
						updateResourceMultiplier(i);

						//printf("Setting scenario faction i = %d [ %s]\n",i,scenarioInfo.factionTypeNames[i].c_str());
						//listBoxFactions[i].setSelectedItem(formatString(scenarioInfo.factionTypeNames[i]));
						setPlayerFactionTypeSelectedItem(i,formatString(scenarioInfo.factionTypeNames[i]));

						//printf("DONE Setting scenario faction i = %d [ %s]\n",i,scenarioInfo.factionTypeNames[i].c_str());

						// Disallow CPU players to be observers
						//if(factionFiles[listBoxFactions[i].getSelectedItemIndex()] == formatString(GameConstants::OBSERVER_SLOTNAME) &&
						if(factionFiles.at(getSelectedPlayerFactionTypeIndex(i)) == formatString(GameConstants::OBSERVER_SLOTNAME) &&
							(getSelectedPlayerControlTypeIndex(i) == ctCpuEasy ||
								getSelectedPlayerControlTypeIndex(i) == ctCpu ||
								getSelectedPlayerControlTypeIndex(i) == ctCpuUltra ||
								getSelectedPlayerControlTypeIndex(i) == ctCpuMega)) {

							//listBoxFactions[i].setSelectedItemIndex(0);
							setPlayerFactionTypeSelectedIndex(i,0);
						}
						//

						setSelectedPlayerTeam(i,intToStr(scenarioInfo.teams[i]));
						//if(factionFiles[listBoxFactions[i].getSelectedItemIndex()] != formatString(GameConstants::OBSERVER_SLOTNAME)) {
						if(factionFiles.at(getSelectedPlayerFactionTypeIndex(i)) != formatString(GameConstants::OBSERVER_SLOTNAME)) {

							if(getSelectedPlayerTeamIndex(i) + 1 != (GameConstants::maxPlayers + fpt_Observer)) {
								lastSelectedTeamIndex[i] = getSelectedPlayerTeamIndex(i);
							}
							// Alow Neutral cpu players
							else if(getSelectedPlayerControlTypeIndex(i) == ctCpuEasy ||
									getSelectedPlayerControlTypeIndex(i) == ctCpu ||
									getSelectedPlayerControlTypeIndex(i) == ctCpuUltra ||
									getSelectedPlayerControlTypeIndex(i) == ctCpuMega) {

								lastSelectedTeamIndex[i] = getSelectedPlayerTeamIndex(i);
							}
						}
						else {
							lastSelectedTeamIndex[i] = -1;
						}
					}

	        		MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	        		bool publishToServerEnabled =
	        				cegui_manager.getCheckboxControlChecked(
	        						cegui_manager.getControl("CheckboxPublishGame"));

					if(publishToServerEnabled) {
						needToRepublishToMasterserver = true;
					}

					if(hasNetworkGameSettings() == true) {
						needToSetChangedGameSettings = true;
						lastSetChangedGameSettings   = time(NULL);;
					}
				}
			}

			updateControlers();
			updateNetworkSlots();

			MutexSafeWrapper safeMutex((publishToMasterserverThread != NULL ? publishToMasterserverThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));
			MutexSafeWrapper safeMutexCLI((publishToClientsThread != NULL ? publishToClientsThread->getMutexThreadObjectAccessor() : NULL),string(__FILE__) + "_" + intToStr(__LINE__));

    		//MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
    		bool publishToServerEnabled =
    				cegui_manager.getCheckboxControlChecked(
    						cegui_manager.getControl("CheckboxPublishGame"));

			if(publishToServerEnabled) {
				needToRepublishToMasterserver = true;
			}
			if(hasNetworkGameSettings() == true) {
				needToSetChangedGameSettings = true;
				lastSetChangedGameSettings   = time(NULL);
			}

			//labelInfo.setText(scenarioInfo.desc);
		}
		else {
			setupMapList("");
			//listBoxMap.setSelectedItem(formatString(formattedPlayerSortedMaps[0][0]));
			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			cegui_manager.setSelectedItemInComboBoxControl(
					cegui_manager.getControl("ComboMap"),formatString(formattedPlayerSortedMaps[0][0]));

			loadMapInfo(Config::getMapPath(getCurrentMapFile(),"",true), &mapInfo, true);
			//labelMapInfo.setText(mapInfo.desc);

			//MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			cegui_manager.setControlText("LabelMapInfo",mapInfo.desc);

			setupTechList("", false);
			reloadFactions(false,"");
			setupTilesetList("");
		}
		SetupUIForScenarios();
	}
	catch(const std::exception &ex) {
		char szBuf[8096]="";
		snprintf(szBuf,8096,"In [%s::%s %d]\nError detected:\n%s\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,ex.what());
		SystemFlags::OutputDebug(SystemFlags::debugError,szBuf);
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s",szBuf);

		mainMessageBoxState=1;
		showMessageBox( szBuf, "Error detected", false);
	}
}

void MenuStateCustomGame::SetupUIForScenarios() {
	try {
		if(getAllowNetworkScenario() == true) {
			// START - Disable changes to controls while in Scenario mode
			for(int i = 0; i < GameConstants::maxPlayers; ++i) {
				//listBoxControls[i].setEditable(false);
				setPlayerControlTypeEnabled(i,false);

				//listBoxFactions[i].setEditable(false);
				//listBoxRMultiplier[i].setEditable(false);
				//listBoxTeams[i].setEditable(false);
			}
			//listBoxFogOfWar.setEditable(false);

			//checkBoxAllowObservers.setEditable(false);
			//listBoxPathFinderType.setEditable(false);
			//checkBoxEnableSwitchTeamMode.setEditable(false);
			//listBoxAISwitchTeamAcceptPercent.setEditable(false);
			//listBoxFallbackCpuMultiplier.setEditable(false);
			//listBoxMap.setEditable(false);
			//listBoxTileset.setEditable(false);
			//listBoxMapFilter.setEditable(false);
			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			cegui_manager.setControlReadOnly(cegui_manager.getControl("ComboMap"),true);
			cegui_manager.setControlReadOnly(cegui_manager.getControl("ComboMapFilter"),true);
			cegui_manager.setControlReadOnly(cegui_manager.getControl("ComboBoxTechTree"),true);
			cegui_manager.setControlReadOnly(cegui_manager.getControl("ComboBoxTileset"),true);
			cegui_manager.setControlReadOnly(cegui_manager.getControl("ComboBoxFogOfWar"),true);

			// END - Disable changes to controls while in Scenario mode
		}
		else {
			// START - Disable changes to controls while in Scenario mode
			for(int i = 0; i < GameConstants::maxPlayers; ++i) {
				//listBoxControls[i].setEditable(true);
				setPlayerControlTypeEnabled(i,true);

				//listBoxFactions[i].setEditable(true);
				//listBoxRMultiplier[i].setEditable(true);
				//listBoxTeams[i].setEditable(true);
			}
			//listBoxFogOfWar.setEditable(true);
			//checkBoxAllowObservers.setEditable(true);
			//listBoxPathFinderType.setEditable(true);
			//checkBoxEnableSwitchTeamMode.setEditable(true);
			//listBoxAISwitchTeamAcceptPercent.setEditable(true);
			//listBoxFallbackCpuMultiplier.setEditable(true);
			//listBoxMap.setEditable(true);
			//listBoxTileset.setEditable(true);
			//listBoxMapFilter.setEditable(true);
			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			cegui_manager.setControlReadOnly(cegui_manager.getControl("ComboMap"),false);
			cegui_manager.setControlReadOnly(cegui_manager.getControl("ComboMapFilter"),false);
			cegui_manager.setControlReadOnly(cegui_manager.getControl("ComboBoxTechTree"),false);
			cegui_manager.setControlReadOnly(cegui_manager.getControl("ComboBoxTileset"),false);
			cegui_manager.setControlReadOnly(cegui_manager.getControl("ComboBoxFogOfWar"),false);

			// END - Disable changes to controls while in Scenario mode
		}
	}
	catch(const std::exception &ex) {
		char szBuf[8096]="";
		snprintf(szBuf,8096,"In [%s::%s %d]\nError detected:\n%s\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,ex.what());
		SystemFlags::OutputDebug(SystemFlags::debugError,szBuf);
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s",szBuf);

		throw megaglest_runtime_error(szBuf);
	}

}

string MenuStateCustomGame::setupMapList(string scenario) {
	string initialMapSelection = "";

	try {
		Config &config = Config::getInstance();
		//MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();

		vector<string> invalidMapList;
		string scenarioDir = Scenario::getScenarioDir(dirList, scenario);
		vector<string> pathList = config.getPathListForType(ptMaps,scenarioDir);
		vector<string> allMaps = MapPreview::findAllValidMaps(pathList,scenarioDir,false,true,&invalidMapList);

		if(scenario != "") {
			vector<string> allMaps2 = MapPreview::findAllValidMaps(config.getPathListForType(ptMaps,""),"",false,true,&invalidMapList);
			copy(allMaps2.begin(), allMaps2.end(), std::inserter(allMaps, allMaps.begin()));
			std::sort(allMaps.begin(),allMaps.end());
		}

		if (allMaps.empty()) {
			throw megaglest_runtime_error("No maps were found!");
		}
		vector<string> results;
		copy(allMaps.begin(), allMaps.end(), std::back_inserter(results));
		mapFiles = results;

		for(unsigned int i = 0; i < GameConstants::maxPlayers+1; ++i) {
			playerSortedMaps[i].clear();
			formattedPlayerSortedMaps[i].clear();
		}

		copy(mapFiles.begin(), mapFiles.end(), std::back_inserter(playerSortedMaps[0]));
		copy(playerSortedMaps[0].begin(), playerSortedMaps[0].end(), std::back_inserter(formattedPlayerSortedMaps[0]));
		std::for_each(formattedPlayerSortedMaps[0].begin(), formattedPlayerSortedMaps[0].end(), FormatString());
		//printf("#5\n");

		for(int i= 0; i < (int)mapFiles.size(); i++){// fetch info and put map in right list
			loadMapInfo(Config::getMapPath(mapFiles.at(i), scenarioDir, false), &mapInfo, false);

			if(GameConstants::maxPlayers+1 <= mapInfo.players) {
				char szBuf[8096]="";
				snprintf(szBuf,8096,"Sorted map list [%d] does not match\ncurrent map playercount [%d]\nfor file [%s]\nmap [%s]",GameConstants::maxPlayers+1,mapInfo.players,Config::getMapPath(mapFiles.at(i), "", false).c_str(),mapInfo.desc.c_str());
				throw megaglest_runtime_error(szBuf);
			}
			playerSortedMaps[mapInfo.players].push_back(mapFiles.at(i));
			formattedPlayerSortedMaps[mapInfo.players].push_back(formatString(mapFiles.at(i)));
			if(config.getString("InitialMap", "Conflict") == formattedPlayerSortedMaps[mapInfo.players].back()){
				initialMapSelection = formattedPlayerSortedMaps[mapInfo.players].back();
			}
		}

		//printf("#6 scenario [%s] [%s]\n",scenario.c_str(),scenarioDir.c_str());
		if(scenario != "") {
			string file = Scenario::getScenarioPath(dirList, scenario);
			loadScenarioInfo(file, &scenarioInfo);

			//printf("#6.1 about to load map [%s]\n",scenarioInfo.mapName.c_str());
			loadMapInfo(Config::getMapPath(scenarioInfo.mapName, scenarioDir, true), &mapInfo, false);

			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			int filterCount = cegui_manager.getItemCountInComboBoxControl(cegui_manager.getControl("ComboMapFilter"));

			if(filterCount > 0) {
				//printf("#6.2\n");
				//listBoxMapFilter.setSelectedItem(intToStr(mapInfo.players));
				cegui_manager.setSelectedItemInComboBoxControl(cegui_manager.getControl("ComboMapFilter"),mapInfo.players);
				//listBoxMap.setItems(formattedPlayerSortedMaps[mapInfo.players]);
				cegui_manager.addItemsToComboBoxControl(
						cegui_manager.getControl("ComboMap"),formattedPlayerSortedMaps[mapInfo.players]);
			}

			//cegui_manager.addItemsToComboBoxControl(
			//		cegui_manager.getControl("ComboMap"), formattedPlayerSortedMaps[mapInfo.players]);

		}
		else {
			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			int filterCount = cegui_manager.getItemCountInComboBoxControl(cegui_manager.getControl("ComboMapFilter"));

			if(filterCount > 0) {
				//printf("Set new map filter to 0: %d\n",filterCount);
				//listBoxMapFilter.setSelectedItemIndex(0);
				cegui_manager.setSelectedItemInComboBoxControl(cegui_manager.getControl("ComboMapFilter"),0);

				//listBoxMap.setItems(formattedPlayerSortedMaps[0]);
				cegui_manager.addItemsToComboBoxControl(
						cegui_manager.getControl("ComboMap"), formattedPlayerSortedMaps[0]);
			}

//			cegui_manager.addItemsToComboBoxControl(
//					cegui_manager.getControl("ComboMap"), formattedPlayerSortedMaps[0]);

		}


		//printf("#7\n");
	}
	catch(const std::exception &ex) {
		char szBuf[8096]="";
		snprintf(szBuf,8096,"In [%s::%s %d]\nError detected:\n%s\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,ex.what());
		SystemFlags::OutputDebug(SystemFlags::debugError,szBuf);
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s",szBuf);

		throw megaglest_runtime_error(szBuf);
		//abort();
	}

	return initialMapSelection;
}

int MenuStateCustomGame::setupTechList(string scenario, bool forceLoad) {
	int initialTechSelection = 0;
	try {
		Config &config = Config::getInstance();

		string scenarioDir = Scenario::getScenarioDir(dirList, scenario);
		vector<string> results;
		vector<string> techPaths = config.getPathListForType(ptTechs,scenarioDir);
		findDirs(techPaths, results);

		if(results.empty()) {
			//throw megaglest_runtime_error("No tech-trees were found!");
			printf("No tech-trees were found (custom)!\n");
		}

		techTreeFiles = results;

		vector<string> translatedTechs;

		for(unsigned int i= 0; i < results.size(); i++) {
			//printf("TECHS i = %d results [%s] scenario [%s]\n",i,results[i].c_str(),scenario.c_str());

			results.at(i)= formatString(results.at(i));
			if(config.getString("InitialTechTree", "Megapack") == results.at(i)) {
				initialTechSelection= i;
			}
			string txTech = techTree->getTranslatedName(techTreeFiles.at(i), forceLoad);
			translatedTechs.push_back(formatString(txTech));
		}


		//listBoxTechTree.setItems(results,translatedTechs);
		MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
		//cegui_manager.addItemsToComboBoxControl(cegui_manager.getControl("ComboBoxTechTree"),results);

		map<string,void*> techTreeList;
		for(unsigned int index = 0; index < results.size(); index++) {
			techTreeList[results[index]] = (void *)translatedTechs[index].c_str();
		}
		cegui_manager.addItemsToComboBoxControl(
				cegui_manager.getControl("ComboBoxTechTree"), techTreeList);

	}
	catch(const std::exception &ex) {
		char szBuf[8096]="";
		snprintf(szBuf,8096,"In [%s::%s %d]\nError detected:\n%s\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,ex.what());
		SystemFlags::OutputDebug(SystemFlags::debugError,szBuf);
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s",szBuf);

		throw megaglest_runtime_error(szBuf);
	}

	return initialTechSelection;
}

void MenuStateCustomGame::reloadFactions(bool keepExistingSelectedItem, string scenario) {
	try {
		Config &config = Config::getInstance();
		Lang &lang= Lang::getInstance();

		vector<string> results;
		string scenarioDir = Scenario::getScenarioDir(dirList, scenario);
		vector<string> techPaths = config.getPathListForType(ptTechs,scenarioDir);

		//printf("#1 techPaths.size() = %d scenarioDir [%s] [%s]\n",techPaths.size(),scenario.c_str(),scenarioDir.c_str());

		MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
		int techTreeCount = cegui_manager.getItemCountInComboBoxControl(cegui_manager.getControl("ComboBoxTechTree"));
		if(techTreeCount > 0) {
			for(int idx = 0; idx < (int)techPaths.size(); idx++) {
				string &techPath = techPaths[idx];
				endPathWithSlash(techPath);

				MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
				int selectedTechtreeIndex = cegui_manager.getSelectedItemIndexFromComboBoxControl(cegui_manager.getControl("ComboBoxTechTree"));

				string factionPath = techPath + techTreeFiles[selectedTechtreeIndex] + "/factions/";
				findDirs(factionPath, results, false, false);

				//printf("idx = %d factionPath [%s] results.size() = %d\n",idx,factionPath.c_str(),results.size());

				if(results.empty() == false) {
					break;
				}
			}
		}

		if(results.empty() == true) {
			//throw megaglest_runtime_error("(2)There are no factions for the tech tree [" + techTreeFiles[listBoxTechTree.getSelectedItemIndex()] + "]");
			showGeneralError = true;

			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			int techTreeCount = cegui_manager.getItemCountInComboBoxControl(cegui_manager.getControl("ComboBoxTechTree"));
			if(techTreeCount > 0) {

				int selectedTechtreeIndex = cegui_manager.getSelectedItemIndexFromComboBoxControl(cegui_manager.getControl("ComboBoxTechTree"));
				generalErrorToShow = "[#2] There are no factions for the tech tree [" + techTreeFiles[selectedTechtreeIndex] + "]";
			}
			else {
				generalErrorToShow = "[#2] There are no factions since there is no tech tree!";
			}
		}

//		results.push_back(formatString(GameConstants::RANDOMFACTION_SLOTNAME));
//
//		// Add special Observer Faction
//		if(checkBoxAllowObservers.getValue() == 1) {
//			results.push_back(formatString(GameConstants::OBSERVER_SLOTNAME));
//		}

		vector<string> translatedFactionNames;
		factionFiles= results;
		for(int i = 0; i < (int)results.size(); ++i) {
			results[i]= formatString(results[i]);

			string translatedString = "";

			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			int techTreeCount = cegui_manager.getItemCountInComboBoxControl(cegui_manager.getControl("ComboBoxTechTree"));
			if(techTreeCount > 0) {

				int selectedTechtreeIndex = cegui_manager.getSelectedItemIndexFromComboBoxControl(cegui_manager.getControl("ComboBoxTechTree"));
				translatedString = techTree->getTranslatedFactionName(techTreeFiles[selectedTechtreeIndex],factionFiles.at(i));
			}
			//printf("translatedString=%s  formatString(results[i])=%s \n",translatedString.c_str(),formatString(results[i]).c_str() );
			if(toLower(translatedString)==toLower(results[i])){
				translatedFactionNames.push_back(results[i]);
			}
			else {
				translatedFactionNames.push_back(results[i]+" ("+translatedString+")");
			}
			//printf("FACTIONS i = %d results [%s]\n",i,results[i].c_str());

			//if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"Tech [%s] has faction [%s]\n",techTreeFiles[listBoxTechTree.getSelectedItemIndex()].c_str(),results[i].c_str());
		}
		results.push_back(formatString(GameConstants::RANDOMFACTION_SLOTNAME));
		factionFiles.push_back(formatString(GameConstants::RANDOMFACTION_SLOTNAME));
		translatedFactionNames.push_back("*"+lang.getString("Random","",true)+"*");

		// Add special Observer Faction
		if(getAllowObservers()) {
			results.push_back(formatString(GameConstants::OBSERVER_SLOTNAME));
			factionFiles.push_back(formatString(GameConstants::OBSERVER_SLOTNAME));
			translatedFactionNames.push_back("*"+lang.getString("Observer","",true)+"*");
		}

		for(int i = 0; i < GameConstants::maxPlayers; ++i) {

			int originalIndex = getSelectedPlayerFactionTypeIndex(i);
			string originalValue = (getSelectedPlayerFactionTypeItemCount(i) > 0 ? getPlayerFactionTypeSelectedItem(i) : "");

			//listBoxFactions[i].setItems(results,translatedFactionNames);

			//ComboBoxPlayer1Multiplier
			//ComboBoxPlayer1Faction

			MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
			cegui_manager.setCurrentLayout("CustomGameMenu.layout",containerName);

			cegui_manager.addItemsToComboBoxControl(
					cegui_manager.getControl("ComboBoxPlayer" + intToStr(i+1) + "Faction"), results);

			if( keepExistingSelectedItem == false ||
				(getAllowObservers() == false &&
						originalValue == formatString(GameConstants::OBSERVER_SLOTNAME)) ) {

				//listBoxFactions[i].setSelectedItemIndex(i % results.size());
				setPlayerFactionTypeSelectedIndex(i, i % results.size());

				//cegui_manager.setSelectedItemInComboBoxControl(
				//	cegui_manager.getControl("ComboBoxPlayer" + intToStr(i+1) + "Faction"), i % results.size());

				if( originalValue == formatString(GameConstants::OBSERVER_SLOTNAME) &&
						getPlayerFactionTypeSelectedItem(i) != formatString(GameConstants::OBSERVER_SLOTNAME)) {
					if(getPlayerTeamSelectedItem(i) == intToStr(GameConstants::maxPlayers + fpt_Observer)) {
						setSelectedPlayerTeam(i,intToStr(1));
					}
				}
			}
			else if(originalIndex < (int)results.size()) {
				//listBoxFactions[i].setSelectedItemIndex(originalIndex);
				setPlayerFactionTypeSelectedIndex(i, originalIndex);
			}
		}
	}
	catch(const std::exception &ex) {
		char szBuf[8096]="";
		snprintf(szBuf,8096,"In [%s::%s %d]\nError detected:\n%s\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,ex.what());
		SystemFlags::OutputDebug(SystemFlags::debugError,szBuf);
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s",szBuf);

		throw megaglest_runtime_error(szBuf);
	}
}

void MenuStateCustomGame::setSlotHuman(int i) {
	if(getPlayerNameEnabled(i)) {
		return;
	}
	//listBoxControls[i].setSelectedItemIndex(ctHuman);
	setPlayerControlTypeSelectedIndex(i,ctHuman);

	//listBoxRMultiplier[i].setSelectedItem("1.0");
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	cegui_manager.setSpinnerControlValue(
					cegui_manager.getControl(
							"SpinnerPlayer" + intToStr(i+1) + "Multiplier"),
								1.0);

//	printf("FILE: [%s: %d] value: %f\n",__FILE__,__LINE__,
//			cegui_manager.getSpinnerControlValue(cegui_manager.getControl(
//					"SpinnerPlayer" + intToStr(i+1) + "Multiplier")));

	setPlayerNameText(i,getHumanPlayerName());
	for(int j = 0; j < GameConstants::maxPlayers; ++j) {
		setPlayerNameEnabled(j,false);
	}
	setPlayerNameEnabled(i,true);
}

void MenuStateCustomGame::setupTilesetList(string scenario) {
	try {
		Config &config = Config::getInstance();

		string scenarioDir = Scenario::getScenarioDir(dirList, scenario);

		vector<string> results;
		findDirs(config.getPathListForType(ptTilesets,scenarioDir), results);
		if (results.empty()) {
			throw megaglest_runtime_error("No tile-sets were found!");
		}
		tilesetFiles= results;
		std::for_each(results.begin(), results.end(), FormatString());

		//listBoxTileset.setItems(results);

		MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
		cegui_manager.addItemsToComboBoxControl(
				cegui_manager.getControl("ComboBoxTileset"), results);
	}
	catch(const std::exception &ex) {
		char szBuf[8096]="";
		snprintf(szBuf,8096,"In [%s::%s %d]\nError detected:\n%s\n",extractFileFromDirectoryPath(__FILE__).c_str(),__FUNCTION__,__LINE__,ex.what());
		SystemFlags::OutputDebug(SystemFlags::debugError,szBuf);
		if(SystemFlags::getSystemSettingType(SystemFlags::debugSystem).enabled) SystemFlags::OutputDebug(SystemFlags::debugSystem,"%s",szBuf);

		throw megaglest_runtime_error(szBuf);
	}

}

int MenuStateCustomGame::getSelectedPlayerControlTypeIndex(int index) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *slotCtl = cegui_manager.getControl(
					"ComboBoxPlayer" + intToStr(index+1) + "Control");
	int selectedSlotControlItemIndex = cegui_manager.
			getSelectedItemIndexFromComboBoxControl(slotCtl);

	return selectedSlotControlItemIndex;
}

int MenuStateCustomGame::getSelectedPlayerControlTypeItemCount(int index) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *slotCtl = cegui_manager.getControl(
					"ComboBoxPlayer" + intToStr(index+1) + "Control");
	int itemCount = cegui_manager.
			getItemCountInComboBoxControl(slotCtl);

	return itemCount;
}

void MenuStateCustomGame::setPlayerControlTypeVisible(int index, bool visible) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl(
					"ComboBoxPlayer" + intToStr(index+1) + "Control");

	cegui_manager.setControlVisible(ctl,visible);
}

void MenuStateCustomGame::setPlayerControlTypeEnabled(int index, bool enabled) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl(
					"ComboBoxPlayer" + intToStr(index+1) + "Control");

	cegui_manager.setControlEnabled(ctl,enabled);
}

void MenuStateCustomGame::setPlayerControlTypeSelectedIndex(int index, int indexValue) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl(
					"ComboBoxPlayer" + intToStr(index+1) + "Control");

	cegui_manager.setSelectedItemInComboBoxControl(ctl,indexValue);
}

void MenuStateCustomGame::setPlayerNumberVisible(int index, bool visible) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl(
					"LabelPlayer" + intToStr(index+1) + "Number");

	cegui_manager.setControlVisible(ctl,visible);
}

void MenuStateCustomGame::setPlayerNameText(int index, string text) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl(
					"EditboxPlayer" + intToStr(index+1) + "Name");

	std::map<int,Texture2D *> &crcPlayerTextureCache =
				CacheManager::getCachedItem< std::map<int,Texture2D *> >(
						GameConstants::playerTextureCacheLookupKey);
	Vec3f playerColor = crcPlayerTextureCache[index]->getPixmap()->getPixel3f(0, 0);
	cegui_manager.setControlTextColor(ctl,playerColor.x, playerColor.y, playerColor.z);
	cegui_manager.setControlText(ctl,text);
}

string MenuStateCustomGame::getPlayerNameText(int index) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl(
					"EditboxPlayer" + intToStr(index+1) + "Name");

	return cegui_manager.getControlText(ctl);
}

void MenuStateCustomGame::setPlayerNameVisible(int index, bool visible) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl(
					"EditboxPlayer" + intToStr(index+1) + "Name");

	if(cegui_manager.getControlVisible(ctl) != visible) {
		cegui_manager.setControlVisible(ctl,visible);
	}
}

bool MenuStateCustomGame::getPlayerNameEnabled(int index) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl(
					"EditboxPlayer" + intToStr(index+1) + "Name");

	return cegui_manager.getControlEnabled(ctl);

}

void MenuStateCustomGame::setPlayerNameEnabled(int index, bool enabled) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl(
					"EditboxPlayer" + intToStr(index+1) + "Name");

	if(cegui_manager.getControlEnabled(ctl) != enabled) {
		cegui_manager.setControlEnabled(ctl,enabled);
		//cegui_manager.setControlReadOnly(ctl,enabled);
	}

}

void MenuStateCustomGame::setPlayerVersionVisible(int index, bool visible) {

	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl(
					"LabelPlayer" + intToStr(index+1) + "Version");

	if(cegui_manager.getControlVisible(ctl) != visible) {
		cegui_manager.setControlVisible(ctl,visible);
	}
}

void MenuStateCustomGame::setPlayerStatusImage(int index, Texture2D *texture) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	string textureName 	= "ImagePlayer" + intToStr(index+1) + "Status_Texture";
	string ctlName 		= "ImagePlayer" + intToStr(index+1) + "Status";

	cegui_manager.setImageForControl(textureName,texture, ctlName, true);
}

void MenuStateCustomGame::setPlayerStatusImageVisible(int index, bool visible) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	string ctlName 		= "ImagePlayer" + intToStr(index+1) + "Status";

	CEGUI::Window *ctl = cegui_manager.getControl(ctlName);

	if(cegui_manager.getControlVisible(ctl) != visible) {
		cegui_manager.setControlVisible(ctl,visible);
	}

}

double MenuStateCustomGame::convertMultiplierIndexToValue(int index) {
//	for(int i=0; i<45; ++i){
//		rMultiplier.push_back(floatToStr(0.5f+0.1f*i,1));
//	}

	return (0.5f + 0.1f * index);
}

int MenuStateCustomGame::convertMultiplierValueToIndex(double value) {
	return (value / 0.1 - 5.0f);
//	for(int index = 0; index < 45; ++index) {
//		if(value == convertMultiplierIndexToValue(index)) {
//			return index;
//		}
//	}
//	throw megaglest_runtime_error("Invalid multiplier value!");
}

int MenuStateCustomGame::getSelectedPlayerFactionTypeIndex(int index) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *slotCtl = cegui_manager.getControl(
					"ComboBoxPlayer" + intToStr(index+1) + "Faction");
	int selectedSlotControlItemIndex = cegui_manager.
			getSelectedItemIndexFromComboBoxControl(slotCtl);

	return selectedSlotControlItemIndex;
}

void MenuStateCustomGame::setPlayerFactionTypeSelectedIndex(int index, int indexValue) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl(
					"ComboBoxPlayer" + intToStr(index+1) + "Faction");

	cegui_manager.setSelectedItemInComboBoxControl(ctl,indexValue);
}

void MenuStateCustomGame::setPlayerFactionTypeSelectedItem(int index, string value) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl(
					"ComboBoxPlayer" + intToStr(index+1) + "Faction");

	cegui_manager.setSelectedItemInComboBoxControl(ctl,value);
}

int MenuStateCustomGame::getSelectedPlayerFactionTypeItemCount(int index) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl(
					"ComboBoxPlayer" + intToStr(index+1) + "Faction");

	int itemCount = cegui_manager.
			getItemCountInComboBoxControl(ctl);

	return itemCount;
}

string MenuStateCustomGame::getPlayerFactionTypeSelectedItem(int index) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl(
					"ComboBoxPlayer" + intToStr(index+1) + "Faction");

	return cegui_manager.getSelectedItemFromComboBoxControl(ctl);
}

int MenuStateCustomGame::hasPlayerFactionTypeItem(int index, string value) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl(
					"ComboBoxPlayer" + intToStr(index+1) + "Faction");

	return cegui_manager.hasItemInComboBoxControl(ctl, value);
}

int MenuStateCustomGame::getSelectedPlayerTeamIndex(int index) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *slotCtl = cegui_manager.getControl(
					"ComboBoxPlayer" + intToStr(index+1) + "Team");
	int selectedSlotControlItemIndex = cegui_manager.
			getSelectedItemIndexFromComboBoxControl(slotCtl);

	return selectedSlotControlItemIndex;

}

void MenuStateCustomGame::setPlayerTeamVisible(int index, bool visible) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl(
					"ComboBoxPlayer" + intToStr(index+1) + "Team");

	if(cegui_manager.getControlVisible(ctl) != visible) {
		cegui_manager.setControlVisible(ctl,visible);
	}
}

void MenuStateCustomGame::setSelectedPlayerTeamIndex(int index, int indexValue) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl(
					"ComboBoxPlayer" + intToStr(index+1) + "Team");

	cegui_manager.setSelectedItemInComboBoxControl(ctl,indexValue);
}

void MenuStateCustomGame::setSelectedPlayerTeam(int index, string value) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl(
					"ComboBoxPlayer" + intToStr(index+1) + "Team");

	cegui_manager.setSelectedItemInComboBoxControl(ctl,value);
}

string MenuStateCustomGame::getPlayerTeamSelectedItem(int index) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl(
					"ComboBoxPlayer" + intToStr(index+1) + "Team");

	return cegui_manager.getSelectedItemFromComboBoxControl(ctl);
}

bool MenuStateCustomGame::getAllowSwitchTeams() {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	return cegui_manager.getCheckboxControlChecked(
			cegui_manager.getControl("CheckboxAllowSwitchTeams"));
}

void MenuStateCustomGame::setAllowSwitchTeams(bool value) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	cegui_manager.setCheckboxControlChecked(
			cegui_manager.getControl("CheckboxAllowSwitchTeams"),value);
}

bool MenuStateCustomGame::getAllowObservers() {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	return cegui_manager.getCheckboxControlChecked(
			cegui_manager.getControl("CheckboxAllowObservers"));
}

void MenuStateCustomGame::setAllowObservers(bool value) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	cegui_manager.setCheckboxControlChecked(
			cegui_manager.getControl("CheckboxAllowObservers"),value);
}

bool MenuStateCustomGame::getAllowInGameJoinPlayer() {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	return cegui_manager.getCheckboxControlChecked(
			cegui_manager.getControl("CheckboxAllowInProgressJoinGame"));
}

void MenuStateCustomGame::setAllowInGameJoinPlayer(bool value) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	cegui_manager.setCheckboxControlChecked(
			cegui_manager.getControl("CheckboxAllowInProgressJoinGame"),value);
}

bool MenuStateCustomGame::getAllowTeamUnitSharing() {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	return cegui_manager.getCheckboxControlChecked(
			cegui_manager.getControl("CheckboxAllowSharedTeamUnits"));
}

void MenuStateCustomGame::setAllowTeamUnitSharing(bool value) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	cegui_manager.setCheckboxControlChecked(
			cegui_manager.getControl("CheckboxAllowSharedTeamUnits"),value);
}

bool MenuStateCustomGame::getAllowTeamResourceSharing() {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	return cegui_manager.getCheckboxControlChecked(
			cegui_manager.getControl("CheckboxAllowSharedTeamResources"));
}

void MenuStateCustomGame::setAllowTeamResourceSharing(bool value) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	cegui_manager.setCheckboxControlChecked(
			cegui_manager.getControl("CheckboxAllowSharedTeamResources"),value);
}

bool MenuStateCustomGame::getAllowNativeLanguageTechtree() {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	return cegui_manager.getCheckboxControlChecked(
			cegui_manager.getControl("CheckboxTechTreeTranslated"));
}

void MenuStateCustomGame::setAllowNativeLanguageTechtree(bool value) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	cegui_manager.setCheckboxControlChecked(
			cegui_manager.getControl("CheckboxTechTreeTranslated"),value);
}

bool MenuStateCustomGame::getNetworkPauseGameForLaggedClients() {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	return cegui_manager.getCheckboxControlChecked(
			cegui_manager.getControl("CheckboxPauseLaggingClients"));
}

void MenuStateCustomGame::setNetworkPauseGameForLaggedClients(bool value) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	cegui_manager.setCheckboxControlChecked(
			cegui_manager.getControl("CheckboxPauseLaggingClients"),value);
}

bool MenuStateCustomGame::getShowAdvancedOptions() {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	return cegui_manager.getCheckboxControlChecked(
			cegui_manager.getControl("CheckboxShowAdvancedOptions"));
}

void MenuStateCustomGame::setShowAdvancedOptions(bool value) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	cegui_manager.setCheckboxControlChecked(
			cegui_manager.getControl("CheckboxShowAdvancedOptions"),value);
}

int MenuStateCustomGame::getSelectedNetworkScenarioIndex() {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *slotCtl = cegui_manager.getControl("ComboNetworkScenarios");
	int selectedSlotControlItemIndex = cegui_manager.
			getSelectedItemIndexFromComboBoxControl(slotCtl);

	return selectedSlotControlItemIndex;

}

void MenuStateCustomGame::setNetworkScenarioVisible(bool visible) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl("ComboNetworkScenarios");

	//printf("#1 visible = %d\n",visible);
	if(cegui_manager.getControlVisible(ctl) != visible) {
		//printf("#2 visible = %d\n",visible);
		cegui_manager.setControlVisible(ctl,visible);
	}
}

void MenuStateCustomGame::setSelectedNetworkScenarioIndex(int indexValue) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl("ComboNetworkScenarios");

	cegui_manager.setSelectedItemInComboBoxControl(ctl,indexValue);

}
void MenuStateCustomGame::setSelectedNetworkScenario(string value) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl("ComboNetworkScenarios");

	cegui_manager.setSelectedItemInComboBoxControl(ctl,value);
}
string MenuStateCustomGame::getNetworkScenarioSelectedItem() {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	CEGUI::Window *ctl = cegui_manager.getControl("ComboNetworkScenarios");

	return cegui_manager.getSelectedItemFromComboBoxControl(ctl);
}

bool MenuStateCustomGame::getAllowNetworkScenario() {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	return cegui_manager.getCheckboxControlChecked(
			cegui_manager.getControl("CheckboxShowNetworkScenarios"));
}
void MenuStateCustomGame::setAllowNetworkScenario(bool value) {
	MegaGlest_CEGUIManager &cegui_manager = MegaGlest_CEGUIManager::getInstance();
	cegui_manager.setCheckboxControlChecked(
			cegui_manager.getControl("CheckboxShowNetworkScenarios"),value);
}

}}//end namespace
