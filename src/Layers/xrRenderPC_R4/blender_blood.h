#pragma once

// Splats drawn into one page of the blood page cache.
class CBlender_blood_stamp : public IBlender
{
public:
	virtual LPCSTR getComment() { return "INTERNAL: blood stamp"; }
	virtual BOOL canBeDetailed() { return FALSE; }
	virtual BOOL canBeLMAPped() { return FALSE; }
	virtual void Compile(CBlender_Compile& C);
};

// Resident blood pages projected onto the G-buffer albedo.
class CBlender_blood_decal : public IBlender
{
public:
	virtual LPCSTR getComment() { return "INTERNAL: blood decal"; }
	virtual BOOL canBeDetailed() { return FALSE; }
	virtual BOOL canBeLMAPped() { return FALSE; }
	virtual void Compile(CBlender_Compile& C);
};

// GPU droplets written into the G-buffer.
class CBlender_blood_particle : public IBlender
{
public:
	virtual LPCSTR getComment() { return "INTERNAL: blood particles"; }
	virtual BOOL canBeDetailed() { return FALSE; }
	virtual BOOL canBeLMAPped() { return FALSE; }
	virtual void Compile(CBlender_Compile& C);
};
