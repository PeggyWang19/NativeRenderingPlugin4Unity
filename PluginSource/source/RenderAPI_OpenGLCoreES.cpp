#include "RenderAPI.h"
#include "PlatformBase.h"

// OpenGL Core profile (desktop) or OpenGL ES (mobile) implementation of RenderAPI.
// Supports several flavors: Core, ES2, ES3

#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <iostream>

#include <android/log.h>
#define DEBUG 1 //日志开关，1为开，其它为关
#if(DEBUG==1)
#define LOG_TAG "OPENGLES_JNI"
#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE,TAG,__VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGV(...) NULL
#define LOGD(...) NULL
#define LOGI(...) NULL
#define LOGE(...) NULL
#endif 


#if SUPPORT_OPENGL_UNIFIED


#include <assert.h>
#if UNITY_IOS || UNITY_TVOS
#	include <OpenGLES/ES2/gl.h>
#elif UNITY_ANDROID || UNITY_WEBGL
#	include <GLES2/gl2.h>
#elif UNITY_OSX
#	include <OpenGL/gl3.h>
#elif UNITY_WIN
// On Windows, use gl3w to initialize and load OpenGL Core functions. In principle any other
// library (like GLEW, GLFW etc.) can be used; here we use gl3w since it's simple and
// straightforward.
#	include "gl3w/gl3w.h"
#elif UNITY_LINUX
#	define GL_GLEXT_PROTOTYPES
#	include <GL/gl.h>
#elif UNITY_EMBEDDED_LINUX
#	include <GLES2/gl2.h>
#if SUPPORT_OPENGL_CORE
#	define GL_GLEXT_PROTOTYPES
#	include <GL/gl.h>
#endif
#else
#	error Unknown platform
#endif


// Frame for rendering
class Frame {
	public:
	double pts;
	unsigned char *data;

	public:
	Frame(double t, unsigned char *d) {
		pts = t;
		data = d;
	}

	~Frame() {
		delete []data;
	}
};

class RenderAPI_OpenGLCoreES : public RenderAPI
{
public:
	RenderAPI_OpenGLCoreES(UnityGfxRenderer apiType);
	virtual ~RenderAPI_OpenGLCoreES() { }

	virtual void ProcessDeviceEvent(UnityGfxDeviceEventType type, IUnityInterfaces* interfaces);

	virtual bool GetUsesReverseZ() { return false; }

	virtual void DrawSimpleTriangles(const float worldMatrix[16], int triangleCount, const void* verticesFloat3Byte4);

	virtual void* BeginModifyTexture(void* textureHandle, int textureWidth, int textureHeight, int* outRowPitch);
	virtual void EndModifyTexture(void* textureHandle, int textureWidth, int textureHeight, int rowPitch, void* dataPtr);

	virtual void* BeginModifyVertexBuffer(void* bufferHandle, size_t* outBufferSize);
	virtual void EndModifyVertexBuffer(void* bufferHandle);

private:
	void CreateResources();
	
	// thread refresh texture image
	void initFrameThread();
	void runFrameThread();
	Frame *getFrameFromThread();
	void stopFrameThread();


private:
	UnityGfxRenderer m_APIType;
	GLuint m_VertexShader;
	GLuint m_FragmentShader;
	GLuint m_Program;
	GLuint m_VertexArray;
	GLuint m_VertexBuffer;
	GLuint m_Texture;
	int m_UniformWorldMatrix;
	int m_UniformProjMatrix;
	int m_UniformTexture;

	int cnt;

public:
	// thread for image transmission
	std::thread * m_thread;
	std::mutex m_mtx;
	std::condition_variable m_con_full;
	std::condition_variable m_con_empty;
	std::queue<Frame *> m_buffer;
	int state = 0;
	int data_width;
	int data_height;
};


RenderAPI* CreateRenderAPI_OpenGLCoreES(UnityGfxRenderer apiType)
{
	return new RenderAPI_OpenGLCoreES(apiType);
}


enum VertexInputs
{
	kVertexInputPosition = 0,
	kVertexInputColor = 1,
	kVertexInputTexture = 2
};


// Simple vertex shader source
// #define VERTEX_SHADER_SRC(ver, attr, varying)						\
// 	ver																\
// 	attr " highp vec3 pos;\n"										\
// 	attr " lowp vec4 color;\n"										\
// 	"\n"															\
// 	varying " lowp vec4 ocolor;\n"									\
// 	"\n"															\
// 	"uniform highp mat4 worldMatrix;\n"								\
// 	"uniform highp mat4 projMatrix;\n"								\
// 	"\n"															\
// 	"void main()\n"													\
// 	"{\n"															\
// 	"	gl_Position = (projMatrix * worldMatrix) * vec4(pos,1);\n"	\
// 	"	ocolor = color;\n"											\
// 	"}\n"															\

#define VERTEX_SHADER_SRC(ver, attr, varying)						\
	ver																\
	attr " highp vec3 pos;\n"										\
	attr " lowp vec4 color;\n"										\
	attr " highp vec2 texCoord;\n"									\
	"\n"															\
	varying " lowp vec4 ocolor;\n"									\
	varying " highp vec2 TexCoord;\n"								\
	"\n"															\
	"uniform highp mat4 worldMatrix;\n"								\
	"uniform highp mat4 projMatrix;\n"								\
	"\n"															\
	"void main()\n"													\
	"{\n"															\
	"	gl_Position = (projMatrix * worldMatrix) * vec4(pos,1);\n"	\
	"	ocolor = color;\n"											\
	"	TexCoord = vec2(texCoord.x, texCoord.y);\n"										\
	"}\n"															\

static const char* kGlesVProgTextGLES2 = VERTEX_SHADER_SRC("\n", "attribute", "varying");
static const char* kGlesVProgTextGLES3 = VERTEX_SHADER_SRC("#version 300 es\n", "in", "out");
#if SUPPORT_OPENGL_CORE
static const char* kGlesVProgTextGLCore = VERTEX_SHADER_SRC("#version 150\n", "in", "out");
#endif

#undef VERTEX_SHADER_SRC


// Simple fragment shader source

// #define FRAGMENT_SHADER_SRC(ver, varying, outDecl, outVar)	\
// 	ver												\
// 	outDecl											\
// 	varying " lowp vec4 ocolor;\n"					\
// 	"\n"											\
// 	"void main()\n"									\
// 	"{\n"											\
// 	"	" outVar " = ocolor;\n"\
// 	"}\n"											\


#define FRAGMENT_SHADER_SRC(ver, varying, outDecl, outVar)	\
	ver												\
	outDecl											\
	varying " lowp vec4 ocolor;\n"					\
	varying " highp vec2 TexCoord;\n"				\
	"uniform lowp sampler2D outTexture;\n" 			\
	"\n"											\
	"void main()\n"									\
	"{\n"											\
	"	" outVar " = texture(outTexture, TexCoord);\n"\
	"}\n"											\

static const char* kGlesFShaderTextGLES2 = FRAGMENT_SHADER_SRC("\n", "varying", "\n", "gl_FragColor");
static const char* kGlesFShaderTextGLES3 = FRAGMENT_SHADER_SRC("#version 300 es\n", "in", "out lowp vec4 fragColor;\n", "fragColor");
#if SUPPORT_OPENGL_CORE
static const char* kGlesFShaderTextGLCore = FRAGMENT_SHADER_SRC("#version 150\n", "in", "out lowp vec4 fragColor;\n", "fragColor");
#endif

#undef FRAGMENT_SHADER_SRC


static GLuint CreateShader(GLenum type, const char* sourceText)
{
	GLuint ret = glCreateShader(type);
	glShaderSource(ret, 1, &sourceText, NULL);
	glCompileShader(ret);
	return ret;
}


void RenderAPI_OpenGLCoreES::CreateResources()
{
	// Make sure that there are no GL error flags set before creating resources
	while (glGetError() != GL_NO_ERROR) {}

	// Create shaders
	if (m_APIType == kUnityGfxRendererOpenGLES20)
	{
		m_VertexShader = CreateShader(GL_VERTEX_SHADER, kGlesVProgTextGLES2);
		m_FragmentShader = CreateShader(GL_FRAGMENT_SHADER, kGlesFShaderTextGLES2);
	}
	else if (m_APIType == kUnityGfxRendererOpenGLES30)
	{
		m_VertexShader = CreateShader(GL_VERTEX_SHADER, kGlesVProgTextGLES3);
		m_FragmentShader = CreateShader(GL_FRAGMENT_SHADER, kGlesFShaderTextGLES3);
	}
#	if SUPPORT_OPENGL_CORE
	else if (m_APIType == kUnityGfxRendererOpenGLCore)
	{
#		if UNITY_WIN
		gl3wInit();
#		endif

		m_VertexShader = CreateShader(GL_VERTEX_SHADER, kGlesVProgTextGLCore);
		m_FragmentShader = CreateShader(GL_FRAGMENT_SHADER, kGlesFShaderTextGLCore);
	}
#	endif // if SUPPORT_OPENGL_CORE


	// Link shaders into a program and find uniform locations
	m_Program = glCreateProgram();
	glBindAttribLocation(m_Program, kVertexInputPosition, "pos");
	glBindAttribLocation(m_Program, kVertexInputColor, "color");
	glBindAttribLocation(m_Program, kVertexInputTexture, "texCoord");
	glAttachShader(m_Program, m_VertexShader);
	glAttachShader(m_Program, m_FragmentShader);
#	if SUPPORT_OPENGL_CORE
	if (m_APIType == kUnityGfxRendererOpenGLCore)
		glBindFragDataLocation(m_Program, 0, "fragColor");
#	endif // if SUPPORT_OPENGL_CORE
	glLinkProgram(m_Program);

	GLint status = 0;
	glGetProgramiv(m_Program, GL_LINK_STATUS, &status);
	assert(status == GL_TRUE);

	m_UniformWorldMatrix = glGetUniformLocation(m_Program, "worldMatrix");
	m_UniformProjMatrix = glGetUniformLocation(m_Program, "projMatrix");

	m_UniformTexture = glGetUniformLocation(m_Program, "outTexture");

	// Create vertex buffer
	glGenBuffers(1, &m_VertexBuffer);
	glBindBuffer(GL_ARRAY_BUFFER, m_VertexBuffer);
	// 认为1024开的足够大了
	glBufferData(GL_ARRAY_BUFFER, 1024, NULL, GL_STREAM_DRAW);


	// Create texture
	glGenTextures(1, &m_Texture);
	glBindTexture(GL_TEXTURE_2D, m_Texture);
	// set parameters for wrapping/filtering
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	unsigned char *texture_data = new unsigned char[200*200*3];
	for(int i = 0; i < 200*200*3 ; i = i + 3){
		texture_data[i] = 0x00;
		texture_data[i + 1] = 0xff;
		texture_data[i + 2] = 0x00;
	}

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 200, 200, 0, GL_RGB, GL_UNSIGNED_BYTE, texture_data);
    glGenerateMipmap(GL_TEXTURE_2D);


	delete []texture_data;


	assert(glGetError() == GL_NO_ERROR);
}

void generateFrame_thread(RenderAPI_OpenGLCoreES * glapi) {
	bool running = glapi->state;
	int cnt = 0;
	int size;
	unsigned char *data;
	Frame *f;

	int d_width = glapi->data_width;
	int d_height = glapi->data_height;
	int MAX_BUFFER_SIZE = 20;

	LOGD("frame thread start: d_width %d, d_height %d\n", d_width, d_height);


	while(running) {
		cnt += 1;

		// test: refresh color every 60 rounds
		// generate new texture and free it when used in glDraw()
		data = new unsigned char[d_width * d_height * 3];
		if(cnt % 60 < 30) {
			for(int i = 0; i < d_width * d_height; ++i) {
				data[i * 3] = 0xff;
				data[i * 3 + 1] = 0;
				data[i * 3 + 2] = 0;
			}
		}
		else {
			for(int i = 0; i < d_width * d_height; i = ++i) {
				data[i * 3] = 0xff;
				data[i * 3 + 1] = 0xff;
				data[i * 3 + 2] = 0;
			}
		}

		LOGD("data addr: %x\n", data);

		f = new Frame(cnt, data);

		std::unique_lock<std::mutex> locker(glapi->m_mtx);
		while(glapi->m_buffer.size() == MAX_BUFFER_SIZE) {
			// unlock m_mtx and wait to be notified
			glapi->m_con_full.wait(locker);
		}
		glapi->m_buffer.push(f);
		glapi->m_con_empty.notify_all();
		locker.unlock();

	}
}


void RenderAPI_OpenGLCoreES::initFrameThread() {

	data_width = 200;
	data_height = 200;
	state = 1;

}

void RenderAPI_OpenGLCoreES::runFrameThread()
{
    // start the image transmission thread
	m_thread = new std::thread(generateFrame_thread, this);
}

void RenderAPI_OpenGLCoreES::stopFrameThread() {

	// end the image transmission thread
	Frame *f;
	while(!m_buffer.empty()) {
		f = m_buffer.front();
		m_buffer.pop();
		delete f;
	}
	m_thread->join();
}

Frame *RenderAPI_OpenGLCoreES::getFrameFromThread() {
	Frame *f;
	
	std::unique_lock<std::mutex> locker(m_mtx);
	while(m_buffer.size() == 0) {
		// unlock m_mtx and wait to be notified
		m_con_empty.wait(locker);
	}
	f = m_buffer.front();
	m_buffer.pop();
	LOGD("getFrameFromThread m_buffer size: %d\n", m_buffer.size());
	m_con_full.notify_all();
	locker.unlock();

	return f;
}



RenderAPI_OpenGLCoreES::RenderAPI_OpenGLCoreES(UnityGfxRenderer apiType)
	: m_APIType(apiType)
{
}


void RenderAPI_OpenGLCoreES::ProcessDeviceEvent(UnityGfxDeviceEventType type, IUnityInterfaces* interfaces)
{
	if (type == kUnityGfxDeviceEventInitialize)
	{
		CreateResources();
		initFrameThread();
		runFrameThread();
	}
	else if (type == kUnityGfxDeviceEventShutdown)
	{
		//@TODO: release resources
		// stopFrameThread();
	}
}


void RenderAPI_OpenGLCoreES::DrawSimpleTriangles(const float worldMatrix[16], int triangleCount, const void* verticesFloat3Byte4)
{

	cnt += 1;
	// Set basic render state
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glDepthFunc(GL_LEQUAL);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_TEXTURE_2D);
	glDepthMask(GL_FALSE);

	// Tweak the projection matrix a bit to make it match what identity projection would do in D3D case.
	float projectionMatrix[16] = {
		1,0,0,0,
		0,1,0,0,
		0,0,2,0,
		0,0,-1,1,
	};

	// Setup shader program to use, and the matrices
	glUseProgram(m_Program);
	glUniformMatrix4fv(m_UniformWorldMatrix, 1, GL_FALSE, worldMatrix);
	glUniformMatrix4fv(m_UniformProjMatrix, 1, GL_FALSE, projectionMatrix);

	// Core profile needs VAOs, setup one
#	if SUPPORT_OPENGL_CORE
	if (m_APIType == kUnityGfxRendererOpenGLCore)
	{
		glGenVertexArrays(1, &m_VertexArray);
		glBindVertexArray(m_VertexArray);
	}
#	endif // if SUPPORT_OPENGL_CORE

	// Bind a vertex buffer, and update data in it
	const int kVertexSize = 12 + 4 + 8;
	// const int kVertexSize = sizeof(verticesFloat3Byte4) / triangleCount;
	// const int kVertexSize = 12 + 4;
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ARRAY_BUFFER, m_VertexBuffer);
	glBufferSubData(GL_ARRAY_BUFFER, 0, kVertexSize * triangleCount * 3, verticesFloat3Byte4);

	// Setup vertex layout
	glEnableVertexAttribArray(kVertexInputPosition);
	glVertexAttribPointer(kVertexInputPosition, 3, GL_FLOAT, GL_FALSE, kVertexSize, (char*)NULL + 0);
	glEnableVertexAttribArray(kVertexInputColor);
	glVertexAttribPointer(kVertexInputColor, 4, GL_UNSIGNED_BYTE, GL_TRUE, kVertexSize, (char*)NULL + 12);
	glEnableVertexAttribArray(kVertexInputTexture);
	glVertexAttribPointer(kVertexInputTexture, 2, GL_FLOAT, GL_FALSE, kVertexSize, (char*)NULL + 12 + 4);



	// Bind Texture and set texture
	// int width = 200;
	// int height = 240;
	// char* data = new char[width*height*3];
	// for(int i = 0; i < width*height; ++i) {
	// 	data[i * 3] = 0xff;
	// 	data[i * 3 + 1] = 0x00;
	// 	data[i * 3 + 2] = 0x00;
	// }

	// if(cnt % 100 == 0){
	// 	for(int i = 0; i < width*height; ++i) {
	// 		data[i * 3] = 0xff;
	// 		data[i * 3 + 1] = 0x00;
	// 		data[i * 3 + 2] = 0xff;
	// 	}
	// }

	Frame *f = getFrameFromThread();
	LOGD("draw frame NO.%f\n", f->pts);

	glBindTexture(GL_TEXTURE_2D, m_Texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, data_width, data_height, 0, GL_RGB, GL_UNSIGNED_BYTE, f->data);
	glGenerateMipmap(GL_TEXTURE_2D);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_Texture);
	glUniform1i(m_UniformTexture, 0);

	// Draw
	glDrawArrays(GL_TRIANGLES, 0, triangleCount * 3);


	delete f;
	glBindTexture(GL_TEXTURE_2D, 0);

	// Cleanup VAO
#	if SUPPORT_OPENGL_CORE
	if (m_APIType == kUnityGfxRendererOpenGLCore)
	{
		glDeleteVertexArrays(1, &m_VertexArray);
	}
#	endif
}


void* RenderAPI_OpenGLCoreES::BeginModifyTexture(void* textureHandle, int textureWidth, int textureHeight, int* outRowPitch)
{
	const int rowPitch = textureWidth * 4;
	// Just allocate a system memory buffer here for simplicity
	unsigned char* data = new unsigned char[rowPitch * textureHeight];
	*outRowPitch = rowPitch;
	return data;
}


void RenderAPI_OpenGLCoreES::EndModifyTexture(void* textureHandle, int textureWidth, int textureHeight, int rowPitch, void* dataPtr)
{
	GLuint gltex = (GLuint)(size_t)(textureHandle);
	// Update texture data, and free the memory buffer
	glBindTexture(GL_TEXTURE_2D, gltex);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, textureWidth, textureHeight, GL_RGBA, GL_UNSIGNED_BYTE, dataPtr);
	delete[](unsigned char*)dataPtr;
}

void* RenderAPI_OpenGLCoreES::BeginModifyVertexBuffer(void* bufferHandle, size_t* outBufferSize)
{
#	if SUPPORT_OPENGL_ES
	return 0;
#	else
	glBindBuffer(GL_ARRAY_BUFFER, (GLuint)(size_t)bufferHandle);
	GLint size = 0;
	glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &size);
	*outBufferSize = size;
	void* mapped = glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
	return mapped;
#	endif
}


void RenderAPI_OpenGLCoreES::EndModifyVertexBuffer(void* bufferHandle)
{
#	if !SUPPORT_OPENGL_ES
	glBindBuffer(GL_ARRAY_BUFFER, (GLuint)(size_t)bufferHandle);
	glUnmapBuffer(GL_ARRAY_BUFFER);
#	endif
}

#endif // #if SUPPORT_OPENGL_UNIFIED
