#ifndef IRENDERABLE_H_INCLUDED
#define IRENDERABLE_H_INCLUDED

#include "render.h"

namespace ShaderBus
{
	struct object_values;
}

//////////////////////////////////////////////////////////////////////////
// definition ("Renderable")
class ENGINE_API IRenderable
{
public:
	struct
	{
		Fmatrix xform;
		IRenderVisual* visual;
		IRender_ObjectSpecific* pROS;
		BOOL pROS_Allowed;
		// own values on shader bus obj_ lanes, null while it has none
		ShaderBus::object_values* bus_values = nullptr;
	} renderable;

public:
	IRenderable();
	virtual ~IRenderable();
	IRender_ObjectSpecific* renderable_ROS();
	BENCH_SEC_SCRAMBLEVTBL2
	virtual void renderable_Render() = 0;
	virtual BOOL renderable_ShadowGenerate() { return FALSE; };
	virtual BOOL renderable_ShadowReceive() { return FALSE; };

	virtual float GetHotness() { return 0.0; }			//--DSR-- HeatVision
	virtual float GetTransparency() { return 0.0; }		//--DSR-- HeatVision
	virtual float GetGlowing() { return 0.0; }			//--DSR-- SilencerOverheat
	// Blood running from wounds, drawn by the object's own shaders
	// (r__blood_body): per wound a row (world centre, radius) and a row
	// (world run vector, wetness). Returns the wound count, at most max_wounds.
	virtual u32 GetBloodWounds(Fvector4* rows, u32 max_wounds) { return 0; }

};

#endif // IRENDERABLE_H_INCLUDED
