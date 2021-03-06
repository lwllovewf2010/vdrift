/************************************************************************/
/*                                                                      */
/* This file is part of VDrift.                                         */
/*                                                                      */
/* VDrift is free software: you can redistribute it and/or modify       */
/* it under the terms of the GNU General Public License as published by */
/* the Free Software Foundation, either version 3 of the License, or    */
/* (at your option) any later version.                                  */
/*                                                                      */
/* VDrift is distributed in the hope that it will be useful,            */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of       */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        */
/* GNU General Public License for more details.                         */
/*                                                                      */
/* You should have received a copy of the GNU General Public License    */
/* along with VDrift.  If not, see <http://www.gnu.org/licenses/>.      */
/*                                                                      */
/************************************************************************/

#ifndef _FBTEXTURE_H
#define _FBTEXTURE_H

#include "texture_interface.h"
#include "glew.h"

#include <iosfwd>

class FrameBufferObject;

class FrameBufferTexture : public TextureInterface
{
friend class FrameBufferObject;
public:
	enum Target
	{
		NORMAL = GL_TEXTURE_2D,
		RECTANGLE = GL_TEXTURE_RECTANGLE,
		CUBEMAP = GL_TEXTURE_CUBE_MAP
	};

	enum CubeSide
	{
		POSX = GL_TEXTURE_CUBE_MAP_POSITIVE_X,
		NEGX = GL_TEXTURE_CUBE_MAP_NEGATIVE_X,
		POSY = GL_TEXTURE_CUBE_MAP_POSITIVE_Y,
		NEGY = GL_TEXTURE_CUBE_MAP_NEGATIVE_Y,
		POSZ = GL_TEXTURE_CUBE_MAP_POSITIVE_Z,
		NEGZ = GL_TEXTURE_CUBE_MAP_NEGATIVE_Z
	};

	enum Format
	{
		LUM8 = GL_LUMINANCE8,
		RGB8 = GL_RGB,
		RGBA8 = GL_RGBA,
		RGB16 = GL_RGB16,
		RGBA16 = GL_RGBA16,
		DEPTH24 = GL_DEPTH_COMPONENT24
	};

	FrameBufferTexture() :
		fbtexture(0),
		renderbuffer_multisample(0),
		inited(false),
		attached(false),
		sizew(0),
		sizeh(0),
		target(NORMAL),
		mipmap(false),
		multisample(0),
		attachment(GL_COLOR_ATTACHMENT0),
		format(RGB8),
		cur_side(POSX),
		depthcomparisonenabled(true)
		{}
	~FrameBufferTexture() {DeInit();}
	void Init(int sizex, int sizey, Target newtarget, Format newformat, bool filternearest, bool usemipmap, std::ostream & error_output, int newmultisample = 0, bool newdepthcomparisonenabled = true);
	void DeInit();
	virtual void Activate() const;
	virtual void Deactivate() const;
	virtual bool Loaded() const {return inited;}
	virtual bool IsRect() const {return (target == RECTANGLE);}
	virtual unsigned int GetW() const {return sizew;}
	virtual unsigned int GetH() const {return sizeh;}
	bool IsCubemap() const {return (target == CUBEMAP);}

private:
	GLuint fbtexture;
	GLuint renderbuffer_multisample;
	bool inited;
	bool attached;
	int sizew, sizeh;

	Target target;
	bool mipmap;
	int multisample;
	int attachment;
	Format format;
	CubeSide cur_side;
	bool depthcomparisonenabled;
};

#endif
