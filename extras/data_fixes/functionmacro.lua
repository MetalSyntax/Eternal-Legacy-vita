-- data/script/functionmacro.lua -- RECONSTRUIDO para el port de PS Vita (Bug #008).
--
-- En el cache de Android disponible este archivo pesa 0 bytes (descarga incompleta). Define
-- los helpers de cutscene que usan ~130 quests; sin el, toda quest con cutscene muere en
-- su primera linea ("attempt to call a nil value") y el juego no avanza (arena, capitulo 0).
--
-- Reconstruido a partir de:
--   * StartCinematic/EndCinematic/FieldNpcTalkScene de data/script/scriptinit.lua (mismo estilo),
--   * como lo usan los 300 scripts de quest (argumentos, que hacen antes y despues),
--   * las funciones EL.* que registra el .so (SetNaviTargetName, AddNPC, DoCameraAni...),
--   * las animaciones de camara de data/3d/level/levelproperty/cam_talk.bdae
--     (cam_talk, cam_side_shot, cam_side_shot_r, cam_up_shot, cam_over_the_shoulder).
-- No es el original: el encuadre exacto de algunas camaras puede diferir.

CAM_TALK_FILE = "data/3d/level/levelproperty/cam_talk.bdae"

-- Nombre de personaje (EL.GetLeaderName) -> indice del nombre en el cuadro de dialogo.
local tCharacterNameIdx = {
	astrian		= TEXT_VALUE_CHARACTER_010,
	lysty		= TEXT_VALUE_CHARACTER_020,
	taric		= TEXT_VALUE_CHARACTER_030,
	edmund		= TEXT_VALUE_CHARACTER_040,
	shira		= TEXT_VALUE_CHARACTER_050,
	fox_master	= TEXT_VALUE_CHARACTER_060,
	reinherz	= TEXT_VALUE_CHARACTER_070,
	cyborg		= TEXT_VALUE_CHARACTER_080,
}

function GetNameStringIdx( sName )
	local idx = tCharacterNameIdx[sName]
	if idx == nil then
		idx = TEXT_VALUE_CHARACTER_010
	end
	return idx
end

-- Entra en modo cutscene: frena al jugador, oculta joystick/navegacion y funde a negro.
-- Los scripts hacen EL.SetUIVisible(SWF_TRANSITION, 0) cuando la escena ya esta armada.
function StartCutscene()
	local uChar = EL.GetObject("char")
	if uChar ~= 0 then
		EL.StopMove( uChar )
		if EL.IsPiyagi( uChar ) == 1 then
			EL.GetOffPiyagi( uChar )
		end
	end

	StartCinematic()
	EL.SetNaviVisible(0)
	EL.SetUIVisible(SWF_TRANSITION, 1)
	EL.WaitTime(300)
end

-- Sale del modo cutscene.
--   sNaviTarget : siguiente objetivo de la navegacion (trigger/portal/NPC) o nil.
--   ...         : NPCs temporales de la escena a borrar (los '' se ignoran).
function EndCutscene( sNaviTarget, ... )
	EL.StopCameraAni()

	for _, sNpc in ipairs({...}) do
		if sNpc ~= '' then
			EL.DeleteNPC( sNpc )
		end
	end

	EL.SetCameraFollow(1)

	if sNaviTarget ~= nil and sNaviTarget ~= '' then
		EL.SetNaviTargetName( sNaviTarget )
	end
	EL.SetNaviVisible(1)

	EL.SetUIVisible(SWF_TRANSITION, 0)
	EndCinematic()
end

-- Crea un NPC de escena.
--   AddNPC(nombre, modelo, {x,y,z})                                   posicion absoluta
--   AddNPC(nombre, modelo, {dx,dy,dz}, sRef, 1, {ox,oy}, sMirar)      relativa a sRef, mirando
--                                                                     a (sMirar + {ox,oy})
function AddNPC( sName, sModel, tPos, sRef, bRelative, tDirOffset, sLookAt )
	local x, y, z = tPos[1], tPos[2], tPos[3]

	if sRef ~= nil and sRef ~= '' and bRelative == 1 then
		local rx, ry, rz = EL.GetPosition( EL.GetObject(sRef) )
		x = x + rx
		y = y + ry
		z = z + rz
	end

	local uNpc = EL.AddNPC( sName, sModel, x, y, z )

	if sLookAt ~= nil and sLookAt ~= '' then
		local lx, ly = EL.GetPosition( EL.GetObject(sLookAt) )
		local ox, oy = 0, 0
		if tDirOffset ~= nil then
			ox = tDirOffset[1] or 0
			oy = tDirOffset[2] or 0
		end
		EL.DirTo( uNpc, lx + ox, ly + oy )
	end

	return uNpc
end

-- Gira el objeto sName hacia el objeto sTarget.
function DirTo( sName, sTarget )
	local tx, ty = EL.GetPosition( EL.GetObject(sTarget) )
	EL.DirTo( EL.GetObject(sName), tx, ty )
end

-- Niebla + distancia de dibujado (mismo orden que los levelscripts).
function SetFog( fFarClip, r, g, b, fStart, fEnd )
	EL.SetCameraFarClip( fFarClip )
	EL.SetFogColor( r, g, b )
	EL.SetFogDistance( fStart, fEnd )
	EL.SetFogEnable(1)
end

-- Dialogo con voz sin tocar camara ni animaciones.
function SetSimpleDialog( iNameIdx, iTextIdx )
	EL.SetDialog_voice( iNameIdx, iTextIdx )
	EL.SetUIVisible( SWF_DIALOG, 1 )
	EL.WaitTalkEnd()
end

-- Dialogo de un personaje:
--   SetDialogAndWait(obj, nombre, texto, aniHabla, loop, aniIdle
--                    [, archivoCamara, aniCamara, usarCamara, alturaCamara])
function SetDialogAndWait( sObj, iNameIdx, iTextIdx, sTalkAni, iLoop, sIdleAni,
                           sCamFile, sCamAni, iUseCam, fCamHeight )
	local uObj = EL.GetObject( sObj )
	local bCam = ( sCamFile ~= nil and sCamFile ~= '' and sCamAni ~= nil and iUseCam ~= 0 )

	if bCam then
		EL.LoadCameraAni( sCamFile )
		EL.DoCameraAni( sCamAni, uObj, fCamHeight or 0 )
	end

	if sTalkAni ~= nil and sTalkAni ~= '' then
		EL.PlayAni( uObj, sTalkAni, iLoop or 1 )
	end

	SetSimpleDialog( iNameIdx, iTextIdx )

	if sIdleAni ~= nil and sIdleAni ~= '' then
		EL.PlayAni( uObj, sIdleAni, 1 )
	end

	if bCam then
		EL.StopCameraAni()
	end
end

function SetDialogAndWait_CamTalk( sObj, iNameIdx, iTextIdx, fCamHeight )
	SetDialogAndWait( sObj, iNameIdx, iTextIdx, "talk", 1, "normal_idle",
	                  CAM_TALK_FILE, "cam_talk", 1, fCamHeight )
end

function SetDialogAndWait_CamSide( sObj, iNameIdx, iTextIdx, fCamHeight )
	SetDialogAndWait( sObj, iNameIdx, iTextIdx, "talk", 1, "normal_idle",
	                  CAM_TALK_FILE, "cam_side_shot", 1, fCamHeight )
end

function SetDialogAndWait_CamSideR( sObj, iNameIdx, iTextIdx, fCamHeight )
	SetDialogAndWait( sObj, iNameIdx, iTextIdx, "talk", 1, "normal_idle",
	                  CAM_TALK_FILE, "cam_side_shot_r", 1, fCamHeight )
end

function SetDialogAndWait_CamUp( sObj, iNameIdx, iTextIdx, fCamHeight )
	SetDialogAndWait( sObj, iNameIdx, iTextIdx, "talk", 1, "normal_idle",
	                  CAM_TALK_FILE, "cam_up_shot", 1, fCamHeight )
end
