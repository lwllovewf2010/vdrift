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

#include "graphics_gl2.h"
#include "graphics_camera.h"
#include "glutil.h"
#include "shader.h"
#include "sky.h"

/// break up the input into a vector of strings using the token characters given
std::vector <std::string> Tokenize(const std::string & input, const std::string & tokens)
{
	std::vector <std::string> out;

	unsigned int pos = 0;
	unsigned int lastpos = 0;

	while (pos != (unsigned int) std::string::npos)
	{
		pos = input.find_first_of(tokens, pos);
		std::string thisstr = input.substr(lastpos,pos-lastpos);
		if (!thisstr.empty())
			out.push_back(thisstr);
		pos = input.find_first_not_of(tokens, pos);
		lastpos = pos;
	}

	return out;
}

static void ReportOnce(const void * id, const std::string & message, std::ostream & output)
{
	static std::map <const void*, std::string> prev_messages;

	std::map <const void*, std::string>::iterator i = prev_messages.find(id);
	if (i == prev_messages.end() || i->second != message)
	{
		prev_messages[id] = message;
		output << message << std::endl;
	}
}

static FrameBufferTexture::Format TextureFormatFromString(const std::string & format)
{
	if (format == "depth" || format == "depthshadow")
		return FrameBufferTexture::DEPTH24;
	else if (format == "luminance8")
		return FrameBufferTexture::LUM8;
	else if (format == "RGBA8")
		return FrameBufferTexture::RGBA8;
	else if (format == "RGB8")
		return FrameBufferTexture::RGB8;
	else if (format == "RGBA16")
		return FrameBufferTexture::RGBA16;
	else if (format == "RGB16")
		return FrameBufferTexture::RGB16;
	else
		assert(0);

	return FrameBufferTexture::RGB8;
}

static bool SortDraworder(Drawable * d1, Drawable * d2)
{
	assert(d1 && d2);
	return (d1->GetDrawOrder() < d2->GetDrawOrder());
}

static std::string BuildKey(const std::string & camera, const std::string & draw)
{
	return camera + ";" + draw;
}

static Quat GetCubeSideOrientation(int i, const Quat & origorient, std::ostream & error_output)
{
	Quat orient = origorient;

	switch (i)
	{
		case 0:
		orient.Rotate(M_PI*0.5, 0,1,0);
		break;

		case 1:
		orient.Rotate(-M_PI*0.5, 0,1,0);
		break;

		case 2:
		orient.Rotate(M_PI*0.5, 1,0,0);
		break;

		case 3:
		orient.Rotate(-M_PI*0.5, 1,0,0);
		break;

		case 4:
		// orient is already set up for us!
		break;

		case 5:
		orient.Rotate(M_PI, 0,1,0);
		break;

		default:
		error_output << "Reached odd spot while building cubemap orientation. How many sides are in a cube, anyway? " << i << "?" << std::endl;
		assert(0);
		break;
	};

	return orient;
}

static void AttachCubeSide(int i, FrameBufferObject & reflection_fbo, std::ostream & error_output)
{
	switch (i)
	{
		case 0:
		reflection_fbo.SetCubeSide(FrameBufferTexture::POSX);
		break;

		case 1:
		reflection_fbo.SetCubeSide(FrameBufferTexture::NEGX);
		break;

		case 2:
		reflection_fbo.SetCubeSide(FrameBufferTexture::POSY);
		break;

		case 3:
		reflection_fbo.SetCubeSide(FrameBufferTexture::NEGY);
		break;

		case 4:
		reflection_fbo.SetCubeSide(FrameBufferTexture::POSZ);
		break;

		case 5:
		reflection_fbo.SetCubeSide(FrameBufferTexture::NEGZ);
		break;

		default:
		error_output << "Reached odd spot while attaching cubemap side. How many sides are in a cube, anyway? " << i << "?" << std::endl;
		assert(0);
		break;
	};

	CheckForOpenGLErrors("cubemap generation: FBO cube side attachment", error_output);
}

static GLint DepthModeFromString(const std::string & mode)
{
	if (mode == "lequal")
		return GL_LEQUAL;
	else if (mode == "equal")
		return GL_EQUAL;
	else if (mode == "gequal")
		return GL_GEQUAL;
	else if (mode == "disabled")
		return GL_ALWAYS;
	else
		assert(0);

	return GL_LEQUAL;
}

static BlendMode::BLENDMODE BlendModeFromString(const std::string & mode)
{
	if (mode == "disabled")
		return BlendMode::DISABLED;
	else if (mode == "add")
		return BlendMode::ADD;
	else if (mode == "alphablend")
		return BlendMode::ALPHABLEND;
	else if (mode == "alphablend_premultiplied")
		return BlendMode::PREMULTIPLIED_ALPHA;
	else if (mode == "alphatest")
		return BlendMode::ALPHATEST;
	else
		assert(0);

	return BlendMode::DISABLED;
}

GraphicsGL2::GraphicsGL2() :
	initialized(false),
	using_shaders(false),
	max_anisotropy(0),
	shadows(false),
	closeshadow(5.0),
	fsaa(1),
	lighting(0),
	bloom(false),
	normalmaps(false),
	contrast(1.0),
	reflection_status(REFLECTION_DISABLED),
	renderconfigfile("noshaders.conf"),
	sky_dynamic(false)
{
	// ctor
}

GraphicsGL2::~GraphicsGL2()
{
	render_outputs.clear();
	texture_outputs.clear();
	texture_inputs.clear();
}

bool GraphicsGL2::Init(
	const std::string & newshaderpath,
	unsigned resx, unsigned resy,
	unsigned bpp, unsigned depthbpp,
	bool fullscreen, unsigned antialiasing,
	bool enableshadows, int new_shadow_distance,
	int new_shadow_quality, int reflection_type,
	const std::string & static_reflectionmap_file,
	const std::string & static_ambientmap_file,
	int anisotropy, int texturesize,
	int lighting_quality, bool newbloom,
	bool newnormalmaps, bool dynamicsky,
	const std::string & renderconfig,
	std::ostream & info_output,
	std::ostream & error_output)
{
	shadows = enableshadows;
	shadow_distance = new_shadow_distance;
	shadow_quality = new_shadow_quality;
	lighting = lighting_quality;
	bloom = newbloom;
	normalmaps = newnormalmaps;
	renderconfigfile = renderconfig;
	shaderpath = newshaderpath;
	sky_dynamic = dynamicsky;

	if (reflection_type == 1)
		reflection_status = REFLECTION_STATIC;
	else if (reflection_type == 2)
		reflection_status = REFLECTION_DYNAMIC;

	ChangeDisplay(resx, resy, error_output);

	fsaa = 1;
	if (antialiasing > 1)
		fsaa = antialiasing;

	if (GLEW_EXT_texture_filter_anisotropic)
		glGetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &max_anisotropy);

	info_output << "Maximum anisotropy: " << max_anisotropy << std::endl;

	if (renderconfigfile == "noshaders.conf")
	{
		DisableShaders(error_output);
	}
	else if (!GLEW_ARB_multitexture)
	{
		info_output << "Your video card doesn't support multitexturing.  Disabling shaders." << std::endl;
		DisableShaders(error_output);
	}
	else if (!GLEW_ARB_texture_cube_map)
	{
		info_output << "Your video card doesn't support cube maps.  Disabling shaders." << std::endl;
		DisableShaders(error_output);
	}
	else if (!GLEW_ARB_texture_non_power_of_two)
	{
		info_output << "Your video card doesn't support non-power-of-two textures.  Disabling shaders." << std::endl;
		DisableShaders(error_output);
	}
	else
	{
		GLint maxattach;
		glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &maxattach);
		info_output << "Maximum color attachments: " << maxattach << std::endl;

		const GLint mrtreq = 1;
		GLint mrt = 0;
		glGetIntegerv(GL_MAX_DRAW_BUFFERS, &mrt);
		info_output << "Maximum draw buffers (" << mrtreq << " required): " << mrt << std::endl;

		#ifndef FBOEXT
		bool use_fbos = GLEW_ARB_framebuffer_object && mrt >= mrtreq && maxattach >= mrtreq;
		#else
		bool use_fbos = GLEW_EXT_framebuffer_object && mrt >= mrtreq && maxattach >= mrtreq;
		#endif

		if (renderconfigfile != "nofbos.conf" && !use_fbos)
		{
			info_output << "Your video card doesn't support framebuffer objects." << std::endl;
			info_output << "Fall back to nofbos.conf." << std::endl;
			renderconfigfile = "nofbos.conf";
		}

		if (GLEW_VERSION_2_0 && GLEW_ARB_shading_language_100 && GLEW_ARB_fragment_shader)
		{
			if ((reflection_status == REFLECTION_STATIC || reflection_status == REFLECTION_DYNAMIC) && !static_reflectionmap_file.empty())
			{
				TextureInfo t;
				t.cube = true;
				t.verticalcross = true;
				t.mipmap = true;
				t.anisotropy = anisotropy;
				t.maxsize = TextureInfo::Size(texturesize);
				static_reflection.Load(static_reflectionmap_file, t, error_output);
			}

			if (!static_ambientmap_file.empty())
			{
				TextureInfo t;
				t.cube = true;
				t.verticalcross = true;
				t.mipmap = false;
				t.anisotropy = anisotropy;
				t.maxsize = TextureInfo::Size(texturesize);
				static_ambient.Load(static_ambientmap_file, t, error_output);
			}

			EnableShaders(info_output, error_output);
		}
		else
		{
			info_output << "Your video card doesn't support shaders. Fall back to noshaders.conf." << std::endl;
			DisableShaders(error_output);
		}
	}

	info_output << "Renderer: " << shaderpath << "/" << renderconfigfile << std::endl;
	initialized = true;
	return true;
}

void GraphicsGL2::Deinit()
{
	if (GLEW_ARB_shading_language_100)
	{
		if (!shadermap.empty())
			glUseProgramObjectARB(0);
		shadermap.clear();
	}
}

void GraphicsGL2::BeginScene(std::ostream & error_output)
{
	glstate.Disable(GL_TEXTURE_2D);
	glstate.Enable(GL_LINE_SMOOTH);
	glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
	glShadeModel(GL_SMOOTH);
	glClearColor(0,0,0,0);
	glClearDepth(1.0f);
	glstate.Enable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
	glstate.Disable(GL_LIGHTING);
	glstate.SetColor(0.5,0.5,0.5,1.0);
	glPolygonOffset(-1.0,-1.0);

	glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	CheckForOpenGLErrors("BeginScene", error_output);
}

void GraphicsGL2::AddDynamicNode(SceneNode & node)
{
	node.Traverse(dynamic_drawlist, Mat4());
}

void GraphicsGL2::AddStaticNode(SceneNode & node)
{
	static_drawlist.Generate(node, false);
}

void GraphicsGL2::ClearDynamicDrawList()
{
	dynamic_drawlist.clear();
}

void GraphicsGL2::ClearStaticDrawList()
{
	static_drawlist.GetDrawList().clear();
}

void GraphicsGL2::SetupScene(
	float fov, float new_view_distance,
	const Vec3 & cam_position,
	const Quat & cam_rotation,
	const Vec3 & dynamic_reflection_sample_pos)
{
	// setup the default camera from the passed-in parameters
	{
		GraphicsCamera & cam = cameras["default"];
		cam.fov = fov;
		cam.pos = cam_position;
		cam.orient = cam_rotation;
		cam.view_distance = new_view_distance;
		cam.w = w;
		cam.h = h;
	}

	// create a camera for the skybox with a long view distance
	{
		GraphicsCamera & cam = cameras["skybox"];
		cam = cameras["default"];
		cam.view_distance = 10000.0;
	}

	// create a camera for the dynamic reflections
	{
		GraphicsCamera & cam = cameras["dynamic_reflection"];
		cam.pos = dynamic_reflection_sample_pos;
		cam.fov = 90; // this gets automatically overridden with the correct fov (which is 90 anyway)
		cam.orient.LoadIdentity(); // this gets automatically rotated for each cube side
		cam.view_distance = 100.f;
		cam.w = 1.f; // this gets automatically overridden with the cubemap dimensions
		cam.h = 1.f; // this gets automatically overridden with the cubemap dimensions
	}

	// create a camera for the dynamic reflection skybox
	{
		GraphicsCamera & cam = cameras["dynamic_reflection_skybox"];
		cam = cameras["dynamic_reflection"];
		cam.view_distance = 10000.f;
	}

	// create an ortho camera for 2d drawing
	{
		GraphicsCamera & cam = cameras["2d"];

		// this is the glOrtho call we want: glOrtho( 0, 1, 1, 0, -1, 1 );
		cam.orthomode = true;
		cam.orthomin = Vec3 (0, 1, -1);
		cam.orthomax = Vec3 (1, 0, 1);
	}

	// put the default camera transform into texture3, needed by shaders only
	Mat4 viewMatrix;
	cam_rotation.GetMatrix4(viewMatrix);
	float translate[4] = {-cam_position[0], -cam_position[1], -cam_position[2], 0};
	viewMatrix.MultiplyVector4(translate);
	viewMatrix.Translate(translate[0], translate[1], translate[2]);

	glMatrixMode(GL_TEXTURE);

	glActiveTexture(GL_TEXTURE3);
	glLoadMatrixf(viewMatrix.GetArray());

	// create cameras for shadow passes
	if (shadows)
	{
		Mat4 viewMatrixInv = viewMatrix.Inverse();

		// derive light rotation quaternion from light direction vector
		Quat light_rotation;
		Vec3 up(0, 0, 1);
		float cosa = up.dot(light_direction);
		if (cosa * cosa < 1.0f)
		{
			float a = -acosf(cosa);
			Vec3 x = up.cross(light_direction).Normalize();
			light_rotation.SetAxisAngle(a, x[0], x[1], x[2]);
		}

		std::vector <std::string> shadow_names;
		shadow_names.push_back("near");
		shadow_names.push_back("medium");
		shadow_names.push_back("far");

		for (int i = 0; i < 3; i++)
		{
			float shadow_radius = (1<<i)*closeshadow+(i)*20.0; //5,30,60

			Vec3 shadowbox(1,1,1);
			shadowbox = shadowbox * (shadow_radius*sqrt(2.0));
			Vec3 shadowoffset(0,0,-1);
			shadowoffset = shadowoffset * shadow_radius;
			(-cam_rotation).RotateVector(shadowoffset);
			shadowbox[2] += 60.0;

			GraphicsCamera & cam = cameras["shadows_"+shadow_names[i]];
			cam = cameras["default"];
			cam.orthomode = true;
			cam.orthomin = -shadowbox;
			cam.orthomax = shadowbox;
			cam.pos = cam.pos + shadowoffset;
			cam.orient = light_rotation;

			// go through and extract the clip matrix, storing it in a texture matrix
			// premultiply the clip matrix with default camera view inverse matrix
			renderscene.SetOrtho(cam.orthomin, cam.orthomax);
			renderscene.SetCameraInfo(cam.pos, cam.orient, cam.fov, cam.view_distance, cam.w, cam.h);

			Mat4 clipmat;
			clipmat.Scale(0.5f);
			clipmat.Translate(0.5f, 0.5f, 0.5f);
			clipmat = renderscene.GetProjMatrix().Multiply(clipmat);
			clipmat = renderscene.GetViewMatrix().Multiply(clipmat);
			clipmat = viewMatrixInv.Multiply(clipmat);

			glActiveTexture(GL_TEXTURE4+i);
			glLoadMatrixf(clipmat.GetArray());
		}
	}

	glActiveTexture(GL_TEXTURE0);
	glMatrixMode(GL_MODELVIEW);
}

void GraphicsGL2::UpdateScene(float dt)
{
	if (sky.get())
	{
		sky->Update(dt);
		SetSunDirection(sky->GetSunDirection());
	}
}

void GraphicsGL2::DrawScene(std::ostream & error_output)
{
	renderscene.SetFlags(using_shaders);
	renderscene.SetFSAA(fsaa);
	renderscene.SetContrast(contrast);
	renderscene.SetSunDirection(light_direction);

	postprocess.SetContrast(contrast);
	postprocess.SetSunDirection(light_direction);

	// sort the two dimentional drawlist so we get correct ordering
	std::sort(dynamic_drawlist.twodim.begin(),dynamic_drawlist.twodim.end(),&SortDraworder);

	// do fast culling queries for static geometry per pass
	std::map <std::string, PtrVector <Drawable> > culled_static_drawlist;
	for (std::vector <GraphicsConfigPass>::const_iterator i = config.passes.begin(); i != config.passes.end(); i++)
	{
		CullScenePass(*i, culled_static_drawlist, error_output);
	}

	// draw the passes
	for (std::vector <GraphicsConfigPass>::const_iterator i = config.passes.begin(); i != config.passes.end(); i++)
	{
		DrawScenePass(*i, culled_static_drawlist, error_output);
	}
}

void GraphicsGL2::EndScene(std::ostream & error_output)
{
	CheckForOpenGLErrors("EndScene", error_output);
}

int GraphicsGL2::GetMaxAnisotropy() const
{
	return max_anisotropy;
}

bool GraphicsGL2::AntialiasingSupported() const
{
	return GLEW_ARB_multisample;
}

bool GraphicsGL2::GetUsingShaders() const
{
	return using_shaders;
}

bool GraphicsGL2::ReloadShaders(std::ostream & info_output, std::ostream & error_output)
{
	EnableShaders(info_output, error_output);

	return GetUsingShaders();
}

void GraphicsGL2::SetCloseShadow(float value)
{
	closeshadow = value;
}

bool GraphicsGL2::GetShadows() const
{
	return shadows;
}

void GraphicsGL2::SetSunDirection(const Vec3 & value)
{
	light_direction = value;
}

void GraphicsGL2::SetContrast(float value)
{
	contrast = value;
}

void GraphicsGL2::SetLocalTime(float hours)
{
	if (sky.get())
		sky->SetTime(hours);
}

void GraphicsGL2::SetLocalTimeSpeed(float value)
{
	if (sky.get())
		sky->SetTimeSpeed(value);
}

GraphicsState & GraphicsGL2::GetState()
{
	return glstate;
}

Shader * GraphicsGL2::GetShader(const std::string & name)
{
	shader_map_type::iterator it = shadermap.find(name);
	if (it != shadermap.end())
		return &it->second;
	else
		return 0;
}

void GraphicsGL2::AddInputTexture(const std::string & name, TextureInterface * texture)
{
	texture_inputs[name] = texture;
}

void GraphicsGL2::ChangeDisplay(
	const int width, const int height,
	std::ostream & error_output)
{
	glViewport(0, 0, (GLint)width, (GLint)height);

	GLfloat ratio = (GLfloat)width / (GLfloat)height;
	Mat4 m;

	glMatrixMode(GL_PROJECTION);
	m.Perspective(45.0f, ratio, 0.1f, 100.0f);
	glLoadMatrixf(m.GetArray());

	glMatrixMode(GL_MODELVIEW);
	m.LoadIdentity();
	glLoadMatrixf(m.GetArray());

	CheckForOpenGLErrors("ChangeDisplay", error_output);

	w = width;
	h = height;
}

bool GraphicsGL2::LoadShader(
	const std::string & shader_name,
	const std::string & shader_defines,
	const std::string & shader_path,
	const std::string & vert_shader_name,
	const std::string & frag_shader_name,
	std::ostream & info_output,
	std::ostream & error_output)
{
	//generate defines
	std::vector <std::string> defines;

	{
		std::stringstream s;
		s << "SCREENRESX " << w;
		defines.push_back(s.str());
	}
	{
		std::stringstream s;
		s << "SCREENRESY " << h;
		defines.push_back(s.str());
	}

	if (reflection_status == REFLECTION_DISABLED)
		defines.push_back("_REFLECTIONDISABLED_");
	else if (reflection_status == REFLECTION_STATIC)
		defines.push_back("_REFLECTIONSTATIC_");
	else if (reflection_status == REFLECTION_DYNAMIC)
		defines.push_back("_REFLECTIONDYNAMIC_");

	if (shadows)
	{
		defines.push_back("_SHADOWS_");
		if (shadow_distance > 0)
			defines.push_back("_CSM2_");
		if (shadow_distance > 1)
			defines.push_back("_CSM3_");
		if (shadow_quality == 0)
			defines.push_back("_SHADOWSLOW_");
		if (shadow_quality == 1)
			defines.push_back("_SHADOWSMEDIUM_");
		if (shadow_quality == 2)
			defines.push_back("_SHADOWSHIGH_");
		if (shadow_quality == 3)
			defines.push_back("_SHADOWSVHIGH_");
		if (shadow_quality == 4)
			defines.push_back("_SHADOWSULTRA_");
	}

	if (normalmaps)
		defines.push_back("_NORMALMAPS_");

	if (lighting == 1)
		defines.push_back("_SSAO_LOW_");

	if (lighting == 2)
		defines.push_back("_SSAO_HIGH_");

	if (!shader_defines.empty())
	{
		std::stringstream s(shader_defines);
		while (s)
		{
			std::string newdefine;
			s >> newdefine;
			if (!newdefine.empty())
				defines.push_back(newdefine);
		}
	}

	std::pair <shader_map_type::iterator, bool> result = shadermap.insert(std::make_pair(shader_name, Shader()));
	assert(result.second);

	Shader & shader = result.first->second;
	bool success = shader.Load(
		shader_path + "/" + vert_shader_name,
		shader_path + "/" + frag_shader_name,
		defines, info_output, error_output);

	return success;
}

void GraphicsGL2::EnableShaders(std::ostream & info_output, std::ostream & error_output)
{
	bool shader_load_success = true;

	CheckForOpenGLErrors("EnableShaders: start", error_output);

	// unload current shaders
	glUseProgramObjectARB(0);
	for (shader_map_type::iterator i = shadermap.begin(); i != shadermap.end(); i++)
	{
		i->second.Unload();
	}
	shadermap.clear();

	CheckForOpenGLErrors("EnableShaders: shader unload", error_output);

	// reload configuration
	config = GraphicsConfig();
	std::string rcpath = shaderpath + "/" + renderconfigfile;
	if (!config.Load(rcpath, error_output))
	{
		error_output << "Error loading render configuration file: " << rcpath << std::endl;
		shader_load_success = false;
	}

	// reload shaders
	std::set <std::string> shadernames;
	for (std::vector <GraphicsConfigShader>::const_iterator s = config.shaders.begin(); s != config.shaders.end(); s++)
	{
		assert(shadernames.find(s->name) == shadernames.end());
		shadernames.insert(s->name);
		shader_load_success = shader_load_success &&
			LoadShader(s->name, s->defines, shaderpath, s->vertex, s->fragment, info_output, error_output);
	}

	CheckForOpenGLErrors("EnableShaders: shader loading", error_output);

	if (!shader_load_success)
	{
		// no shaders fallback
		error_output << "Disabling shaders due to shader loading error" << std::endl;
		DisableShaders(error_output);
		return;
	}

	info_output << "Successfully enabled shaders" << std::endl;
	using_shaders = true;

	// unload current outputs
	render_outputs.clear();
	texture_outputs.clear();
	texture_inputs.clear();

	CheckForOpenGLErrors("EnableShaders: FBO deinit", error_output);

	bool ssao = (lighting > 0);
	bool ssao_low = (lighting == 1);
	bool ssao_high = (lighting == 2);
	bool reflection_disabled = (reflection_status == REFLECTION_DISABLED);
	bool reflection_dynamic = (reflection_status == REFLECTION_DYNAMIC);
	bool shadows_near = shadows;
	bool shadows_medium = shadows && shadow_distance > 0;
	bool shadows_far = shadows && shadow_distance > 1;
	bool shadow_quality_low = shadows && (shadow_quality == 0);
	bool shadow_quality_medium = shadows && (shadow_quality == 1);
	bool shadow_quality_high = shadows && (shadow_quality == 2);
	bool shadow_quality_vhigh = shadows && (shadow_quality == 3);
	bool shadow_quality_ultra = shadows && (shadow_quality == 4);

	// for now, map vhigh and ultra to high
	shadow_quality_high = shadow_quality_high || shadow_quality_vhigh || shadow_quality_ultra;
	shadow_quality_vhigh = false;
	shadow_quality_ultra = true;

	conditions.clear();
	if (fsaa > 1) conditions.insert("fsaa");
	#define ADDCONDITION(x) if (x) conditions.insert(#x)
	ADDCONDITION(bloom);
	ADDCONDITION(normalmaps);
	ADDCONDITION(ssao);
	ADDCONDITION(ssao_low);
	ADDCONDITION(ssao_high);
	ADDCONDITION(reflection_disabled);
	ADDCONDITION(reflection_dynamic);
	ADDCONDITION(shadows_near);
	ADDCONDITION(shadows_medium);
	ADDCONDITION(shadows_far);
	ADDCONDITION(shadow_quality_low);
	ADDCONDITION(shadow_quality_medium);
	ADDCONDITION(shadow_quality_high);
	ADDCONDITION(shadow_quality_vhigh);
// 		ADDCONDITION(shadow_quality_ultra);
	ADDCONDITION(sky_dynamic);
	#undef ADDCONDITION

	// add some common textures
	if (reflection_status == REFLECTION_STATIC)
		texture_inputs["reflection_cube"] = static_reflection;
	texture_inputs["ambient_cube"] = static_ambient;

	bool has_texture_float = GLEW_ARB_texture_float && GLEW_ARB_half_float_pixel;

	for (std::vector <GraphicsConfigOutput>::const_iterator i = config.outputs.begin(); i != config.outputs.end(); i++)
	{
		if (i->conditions.Satisfied(conditions))
		{
			if (texture_outputs.find(i->name) != texture_outputs.end())
			{
				error_output << "Detected duplicate output name in render config: " << i->name << ", only the first output will be constructed." << std::endl;
				continue;
			}

			if (i->type == "framebuffer")
			{
				render_outputs[i->name].RenderToFramebuffer();
			}
			else
			{
				FrameBufferTexture & fbtex = texture_outputs[i->name];
				FrameBufferTexture::Target type = FrameBufferTexture::NORMAL;
				if (i->type == "rectangle")
					type = FrameBufferTexture::RECTANGLE;
				else if (i->type == "cube")
					type = FrameBufferTexture::CUBEMAP;
				int fbms = 0;
				if (i->multisample < 0)
					fbms = fsaa;

				// check texture format
				FrameBufferTexture::Format format = TextureFormatFromString(i->format);
				if (!has_texture_float && (format == FrameBufferTexture::RGBA16 || format == FrameBufferTexture::RGB16))
				{
					error_output << "Your video card doesn't support floating point textures." << std::endl;
					error_output << "Failed to load render output: " << i->name << " " << i->type << std::endl;
					DisableShaders(error_output);
					return;
				}

				// initialize fbtexture
				fbtex.Init(
					i->width.GetSize(w), i->height.GetSize(h),
					type, format, (i->filter == "nearest"), i->mipmap,
					error_output, fbms, (i->format == "depthshadow"));

				// map to input texture
				texture_inputs[i->name] = fbtex;
			}

			info_output << "Initialized render output: " << i->name << (i->type != "framebuffer" ? " (FBO)" : " (framebuffer alias)") << std::endl;
		}
	}

	render_outputs["framebuffer"].RenderToFramebuffer();

	// go through all pass outputs and construct the actual FBOs, which can consist of one or more fbtextures
	for (std::vector <GraphicsConfigPass>::const_iterator i = config.passes.begin(); i != config.passes.end(); i++)
	{
		if (i->conditions.Satisfied(conditions))
		{
			// see if it already exists
			std::string outname = i->output;
			render_output_map_type::iterator curout = render_outputs.find(outname);
			if (curout == render_outputs.end())
			{
				// tokenize the output list
				std::vector <std::string> outputs = Tokenize(outname, " ");

				// collect a list of textures for the outputs
				std::vector <FrameBufferTexture*> fbotex;
				for (std::vector <std::string>::const_iterator o = outputs.begin(); o != outputs.end(); o++)
				{
					texture_output_map_type::iterator to = texture_outputs.find(*o);
					if (to != texture_outputs.end())
					{
						fbotex.push_back(&to->second);
					}
				}

				if (fbotex.empty())
				{
					error_output << "None of these outputs are active: " << error_output << ", this pass will not have an output." << std::endl;
					continue;
				}

				// initialize fbo
				FrameBufferObject & fbo = render_outputs[outname].RenderToFBO();
				fbo.Init(glstate, fbotex, error_output);
			}
		}
	}

	if (sky_dynamic)
	{
		sky.reset(new Sky(*this, error_output));
		texture_inputs["sky"] = sky.get();
		//sky->UpdateComplete();
	}
}

void GraphicsGL2::DisableShaders(std::ostream & error_output)
{
	if (using_shaders)
	{
		glUseProgramObjectARB(0);
	}

	renderconfigfile = "noshaders.conf";
	shadermap.clear();
	using_shaders = false;
	shadows = false;

	// load non-shader configuration
	config = GraphicsConfig();
	std::string rcpath = shaderpath + "/" + renderconfigfile;
	if (!config.Load(rcpath, error_output))
	{
		error_output << "Error loading non-shader render configuration file: " << rcpath << std::endl;
		assert(0); // uh oh, now we're really boned
	}

	render_outputs["framebuffer"].RenderToFramebuffer();

	if (sky_dynamic)
	{
		texture_inputs.erase("sky");
		sky.reset();
	}
}

void GraphicsGL2::CullScenePass(
	const GraphicsConfigPass & pass,
	std::map <std::string, PtrVector <Drawable> > & culled_static_drawlist,
	std::ostream & error_output)
{
	// for each pass, we have which camera and which draw layer to use
	// we want to do culling for each unique camera and draw layer combination
	// use camera/layer as the unique key
	assert(!pass.draw.empty());

	if (pass.draw.back() == "postprocess" || !pass.conditions.Satisfied(conditions))
		return;

	for (std::vector <std::string>::const_iterator d = pass.draw.begin(); d != pass.draw.end(); d++)
	{
		// determine if we're dealing with a cubemap
		render_output_map_type::iterator oi = render_outputs.find(pass.output);

		if (oi == render_outputs.end())
		{
			ReportOnce(&pass, "Render output "+pass.output+" couldn't be found", error_output);
			return;
		}

		bool cubemap = (oi->second.IsFBO() && oi->second.RenderToFBO().IsCubemap());

		std::string cameraname = pass.camera;
		const int cubesides = cubemap ? 6 : 1;

		for (int cubeside = 0; cubeside < cubesides; cubeside++)
		{
			if (cubemap)
			{
				// build sub-camera

				// build a name for the sub camera
				{
					std::stringstream converter;
					converter << pass.camera << "_cubeside" << cubeside;
					cameraname = converter.str();
				}

				// get the base camera
				camera_map_type::iterator bci = cameras.find(pass.camera);

				if (bci == cameras.end())
				{
					ReportOnce(&pass, "Camera "+pass.camera+" couldn't be found", error_output);
					return;
				}

				// create our sub-camera
				GraphicsCamera & cam = cameras[cameraname];
				cam = bci->second;

				// set the sub-camera's properties
				cam.orient = GetCubeSideOrientation(cubeside, cam.orient, error_output);
				cam.fov = 90;
				assert(oi->second.IsFBO());
				const FrameBufferObject & fbo = oi->second.RenderToFBO();
				cam.w = fbo.GetWidth();
				cam.h = fbo.GetHeight();
			}

			std::string key = BuildKey(cameraname, *d);
			if (pass.cull)
			{
				camera_map_type::iterator ci = cameras.find(cameraname);

				if (ci == cameras.end())
				{
					ReportOnce(&pass, "Camera "+cameraname+" couldn't be found", error_output);
					return;
				}

				GraphicsCamera & cam = ci->second;
				if (culled_static_drawlist.find(key) == culled_static_drawlist.end())
				{
					if (cam.orthomode)
						renderscene.SetOrtho(cam.orthomin, cam.orthomax);
					else
						renderscene.DisableOrtho();
					Frustum frustum = renderscene.SetCameraInfo(cam.pos, cam.orient, cam.fov, cam.view_distance, cam.w, cam.h);
					reseatable_reference <AabbTreeNodeAdapter <Drawable> > container =
						static_drawlist.GetDrawList().GetByName(*d);

					if (!container)
					{
						ReportOnce(&pass, "Drawable container "+*d+" couldn't be found", error_output);
						return;
					}

					container->Query(frustum, culled_static_drawlist[key]);
					renderscene.DisableOrtho();
				}
			}
			else
			{
				reseatable_reference <AabbTreeNodeAdapter <Drawable> > container =
					static_drawlist.GetDrawList().GetByName(*d);

				if (!container)
				{
					ReportOnce(&pass, "Drawable container "+*d+" couldn't be found", error_output);
					return;
				}

				container->Query(Aabb<float>::IntersectAlways(), culled_static_drawlist[key]);
			}
		}
	}
}

void GraphicsGL2::DrawScenePass(
	const GraphicsConfigPass & pass,
	std::map <std::string, PtrVector <Drawable> > & culled_static_drawlist,
	std::ostream & error_output)
{
	if (!pass.conditions.Satisfied(conditions))
		return;

	assert(!pass.draw.empty());
	if (pass.draw.back() == "postprocess")
	{
		DrawScenePassPost(pass, error_output);
		return;
	}

	std::vector <TextureInterface*> input_textures;
	GetScenePassInputTextures(pass.inputs, input_textures);

	// setup shader
	if (using_shaders)
	{
		shader_map_type::iterator si = shadermap.find(pass.shader);
		if (si == shadermap.end())
		{
			ReportOnce(&pass, "Shader " + pass.shader + " couldn't be found", error_output);
			return;
		}
		renderscene.SetDefaultShader(si->second);
	}

	// setup render input
	renderscene.SetBlendMode(BlendModeFromString(pass.blendmode));
	renderscene.SetDepthMode(DepthModeFromString(pass.depthtest));
	renderscene.SetClear(pass.clear_color, pass.clear_depth);
	renderscene.SetWriteColor(pass.write_color);
	renderscene.SetWriteAlpha(pass.write_alpha);
	renderscene.SetWriteDepth(pass.write_depth);

	// setup render output
	render_output_map_type::iterator oi = render_outputs.find(pass.output);
	if (oi == render_outputs.end())
	{
		ReportOnce(&pass, "Render output " + pass.output + " couldn't be found", error_output);
		return;
	}

	for (std::vector <std::string>::const_iterator d = pass.draw.begin(); d != pass.draw.end(); d++)
	{
		// draw layer
		DrawScenePassLayer(*d, pass, input_textures, culled_static_drawlist, oi->second, error_output);

		// disable color, zclear
		renderscene.SetClear(false, false);
	}
}

void GraphicsGL2::DrawScenePassPost(
	const GraphicsConfigPass & pass,
	std::ostream & error_output)
{
	assert(pass.draw.back() == "postprocess");

	std::vector <TextureInterface*> input_textures;
	GetScenePassInputTextures(pass.inputs, input_textures);

	// setup camera, even though we don't use it directly for the post process we want to have some info available
	std::string cameraname = pass.camera;
	camera_map_type::iterator ci = cameras.find(cameraname);
	if (ci == cameras.end())
	{
		ReportOnce(&pass, "Camera " + cameraname + " couldn't be found", error_output);
		return;
	}

	GraphicsCamera & cam = ci->second;
	if (cam.orthomode)
		renderscene.SetOrtho(cam.orthomin, cam.orthomax);
	else
		renderscene.DisableOrtho();

	renderscene.SetCameraInfo(cam.pos, cam.orient, cam.fov, cam.view_distance, cam.w, cam.h);

	postprocess.SetCameraInfo(cam.pos, cam.orient, cam.fov, cam.view_distance, cam.w, cam.h);
	postprocess.SetDepthMode(DepthModeFromString(pass.depthtest));
	postprocess.SetWriteDepth(pass.write_depth);
	postprocess.SetClear(pass.clear_color, pass.clear_depth);
	postprocess.SetBlendMode(BlendModeFromString(pass.blendmode));

	shader_map_type::iterator si = shadermap.find(pass.shader);
	if (si == shadermap.end())
	{
		ReportOnce(&pass, "Shader " + pass.shader + " couldn't be found", error_output);
		return;
	}

	RenderPostProcess(
		pass.shader, input_textures,
		render_outputs[pass.output],
		pass.write_color, pass.write_alpha,
		error_output);
}

void GraphicsGL2::GetScenePassInputTextures(
	const GraphicsConfigInputs & inputs,
	std::vector <TextureInterface*> & input_textures)
{
	for (std::map <unsigned int, std::string>::const_iterator t = inputs.tu.begin(); t != inputs.tu.end(); t++)
	{
		unsigned int tuid = t->first;

		unsigned int cursize = input_textures.size();
		for (unsigned int extra = cursize; extra < tuid; extra++)
			input_textures.push_back(NULL);

		const std::string & texname = t->second;

		// quietly ignore invalid names
		// this allows us to specify outputs that are only present for certain conditions
		// and then always specify those outputs as inputs to later stages, and have
		// them be ignored if the conditions aren't met
		if (texture_inputs.find(texname) != texture_inputs.end())
		{
			input_textures.push_back(&*texture_inputs[texname]);
		}
		else
		{
			//TODO: decide if i want to do fancier error detection here to catch typos in render.conf
			//std::cout << "warning: " << texname << " not found" << std::endl;
			input_textures.push_back(NULL);
		}
	}
}

void GraphicsGL2::BindInputTextures(
	const std::vector <TextureInterface*> & textures,
	std::ostream & error_output)
{
	for (unsigned int i = 0; i < textures.size(); i++)
	{
		if (textures[i])
		{
			glActiveTexture(GL_TEXTURE0+i);
			textures[i]->Activate();

			if (CheckForOpenGLErrors("RenderDrawlists extra texture bind", error_output))
			{
				error_output << "this error occurred while binding texture " << i << " loaded=" << textures[i]->Loaded() << std::endl;
			}
		}
	}

	glActiveTexture(GL_TEXTURE0);
}

void GraphicsGL2::UnbindInputTextures(
	const std::vector <TextureInterface*> & textures,
	std::ostream & error_output)
{
	for (unsigned int i = 0; i < textures.size(); i++)
	{
		if (textures[i])
		{
			glActiveTexture(GL_TEXTURE0+i);
			textures[i]->Deactivate();

			if (CheckForOpenGLErrors("RenderDrawlists extra texture unbind", error_output))
			{
				error_output << "this error occurred while binding texture " << i << " loaded=" << textures[i]->Loaded() << std::endl;
			}
		}
	}

	glActiveTexture(GL_TEXTURE0);
}

void GraphicsGL2::DrawScenePassLayer(
	const std::string & layer,
	const GraphicsConfigPass & pass,
	const std::vector <TextureInterface*> & input_textures,
	const std::map <std::string, PtrVector <Drawable> > & culled_static_drawlist,
	RenderOutput & render_output,
	std::ostream & error_output)
{
	// handle the cubemap case
	bool cubemap = (render_output.IsFBO() && render_output.RenderToFBO().IsCubemap());
	std::string cameraname = pass.camera;
	const int cubesides = cubemap ? 6 : 1;

	for (int cubeside = 0; cubeside < cubesides; cubeside++)
	{
		if (cubemap)
		{
			// build a name for the sub camera
			std::stringstream converter;
			converter << pass.camera << "_cubeside" << cubeside;
			cameraname = converter.str();

			// attach the correct cube side on the render output
			AttachCubeSide(cubeside, render_output.RenderToFBO(), error_output);
		}

		// setup camera
		camera_map_type::iterator ci = cameras.find(cameraname);

		if (ci == cameras.end())
		{
			ReportOnce(&pass, "Camera " + pass.camera + " couldn't be found", error_output);
			return;
		}

		GraphicsCamera & cam = ci->second;
		if (cam.orthomode)
			renderscene.SetOrtho(cam.orthomin, cam.orthomax);
		else
			renderscene.DisableOrtho();
		renderscene.SetCameraInfo(cam.pos, cam.orient, cam.fov, cam.view_distance, cam.w, cam.h);

		// setup dynamic drawlist
		reseatable_reference <PtrVector <Drawable> > container_dynamic = dynamic_drawlist.GetByName(layer);
		if (!container_dynamic)
		{
			ReportOnce(&pass, "Drawable container " + layer + " couldn't be found", error_output);
			return;
		}

		// setup static drawlist
		const std::string drawlist_key = BuildKey(cameraname, layer);
		std::map <std::string, PtrVector <Drawable> >::const_iterator container_static =
			culled_static_drawlist.find(drawlist_key);
		if (container_static == culled_static_drawlist.end())
		{
			ReportOnce(&pass, "Couldn't find culled static drawlist for camera/draw combination: " + drawlist_key, error_output);
			return;
		}

		// car paint hack for non-shader path
		bool carhack = !using_shaders && (layer == "car_noblend");
		renderscene.SetCarPaintHack(carhack);

		// render
		RenderDrawlists(
			*container_dynamic,
			container_static->second,
			input_textures,
			renderscene,
			render_output,
			error_output);

		// cleanup
		renderscene.DisableOrtho();
	}
}

void GraphicsGL2::RenderDrawlist(
	const std::vector <Drawable*> & drawlist,
	RenderInputScene & render_scene,
	RenderOutput & render_output,
	std::ostream & error_output)
{
	if (drawlist.empty() && !render_scene.GetClear().first && !render_scene.GetClear().second)
		return;

	std::vector <Drawable*> empty;
	render_scene.SetDrawLists(drawlist, empty);
	Render(&render_scene, render_output, error_output);
}

void GraphicsGL2::RenderDrawlists(
	const std::vector <Drawable*> & dynamic_drawlist,
	const std::vector <Drawable*> & static_drawlist,
	const std::vector <TextureInterface*> & extra_textures,
	RenderInputScene & render_scene,
	RenderOutput & render_output,
	std::ostream & error_output)
{
	if (dynamic_drawlist.empty() && static_drawlist.empty() &&
		!render_scene.GetClear().first && !render_scene.GetClear().second)
		return;

	BindInputTextures(extra_textures, error_output);

	render_scene.SetDrawLists(dynamic_drawlist, static_drawlist);

	Render(&render_scene, render_output, error_output);

	UnbindInputTextures(extra_textures, error_output);
}

void GraphicsGL2::RenderPostProcess(
	const std::string & shadername,
	const std::vector <TextureInterface*> & textures,
	RenderOutput & render_output,
	bool write_color,
	bool write_alpha,
	std::ostream & error_output)
{
	postprocess.SetWriteColor(write_color);
	postprocess.SetWriteAlpha(write_alpha);
	std::map <std::string, Shader>::iterator s = shadermap.find(shadername);
	assert(s != shadermap.end());
	postprocess.SetShader(&s->second);
	postprocess.SetSourceTextures(textures);
	Render(&postprocess, render_output, error_output);
}

void GraphicsGL2::Render(RenderInput * input, RenderOutput & output, std::ostream & error_output)
{
	output.Begin(glstate, error_output);

	CheckForOpenGLErrors("render output begin", error_output);

	input->Render(glstate, error_output);

	CheckForOpenGLErrors("render finish", error_output);

	output.End(glstate, error_output);

	CheckForOpenGLErrors("render output end", error_output);
}

