//-
// Copyright 2015 Autodesk, Inc. All rights reserved.
// 
// Use of this software is subject to the terms of the Autodesk
// license agreement provided at the time of installation or download,
// or which otherwise accompanies this software in either electronic
// or hard copy form.
//+

///////////////////////////////////////////////////////////////////
//
// NOTE: PLEASE READ THE README.TXT FILE FOR INSTRUCTIONS ON
// COMPILING AND USAGE REQUIREMENTS.
//
// File: pnTrianglesNode.cpp
//
// Dependency Graph Node: pnTriangles
//
// Description:
//
//		Hardware shader plugin to perform :
//		1) ATI PN triangle tessellation on geometry, if the extension
//			ATI_pn_triangles is supported in hardware.
//		2) A simple vertex program using EXT_vertex_program, if the
//			extension is supported.
//		3) A simple fragement program using AT_fragment_program, if the
//			extension is supported.
//
//		This is an excerpt from the PN triangle extension
//		specification:
//
//		"When PN Triangle generation is enabled, each triangle-based geometric 
//	    primitive is replaced with a new curved surface using the primitive 
//		vertices as control points.  The intent of PN Triangles 
//		are to take a set of triangle-based geometry and algorithmically 
//		tessellate it into a more organic shape with a smoother silhouette. 
//		The new surface can either linearly or quadratically interpolate the 
//		normals across the patch.  The vertices can be either linearly or
//		cubically interpolated across the patch.  Linear interpolation
//		of the points would be useful for getting more sample points for 
//		lighting on the same geometric shape.  All other vertex information
//		(colors, texture coordinates, fog coordinates, and vertex weights) are 
//		interpolated linearly across the patch."
//
//		This plugin is ATI Radeon specific.
//
///////////////////////////////////////////////////////////////////
#include <maya/MIOStream.h>
#include <math.h>

#include "pnTrianglesNode.h"

#include <maya/MFnPlugin.h>
#include <maya/MString.h>

#include <maya/MPlug.h>
#include <maya/MDataBlock.h>
#include <maya/MDataHandle.h>
#include <maya/MFnNumericAttribute.h>
#include <maya/MFnTypedAttribute.h>
#include <maya/MFloatVector.h>
#include <maya/MGlobal.h>
#include <maya/M3dView.h>
#include <maya/MMaterial.h>
#include <maya/MDrawData.h>
#include <maya/MColor.h>
#include <maya/MDagPath.h>
#include <maya/MPoint.h>
#include <maya/MIOStream.h>

MTypeId     pnTriangles::id( 0x00105448 );

#define		pnDefaultMaxTessellation	0		// Default max tessellation

// Attributes
// 
MObject     pnTriangles::attrSubdivisions;  // Subdivision level      
MObject		pnTriangles::attrColored;		// Display colored
MObject		pnTriangles::attrTextured;		// Display textured
MObject		pnTriangles::attrWireframe;		// Display wireframe
MObject		pnTriangles::attrNormalMode;	// Normal evaluation mode
MObject		pnTriangles::attrPointMode;		// Point evaluation mode
MObject		pnTriangles::attrEnableVertexProgram; // Enable vertex program
MObject		pnTriangles::attrEnablePixelProgram; // Enable pixel program

pnTriangles::pnTriangles() 
:	fSubdivisions( 0 ),									// Default 0 level of subdivision
	fNumTextureCoords( 0 ),								// Default not-textured
	fNumNormals( 1 ),									// Default shaded
	fNumColors( 0 ),									// Default not-colored
	fPointMode( kPointCubic ),							// Default cubic
	fNormalMode( kNormalQuadratic ),					// Default quadratic
	fMaxTessellationLevel( pnDefaultMaxTessellation ),	// 0 maximum default
	fWireframe( false ),								// Default filled
	fInTexturedMode( false ),							// Keep track of textured mode
	fTestVertexProgram( false ),
	fTestFragmentProgram( false ),
	fVertexShaderId( -1 ),
	fFragmentShaderId( -1 )
{
	fExtensionSupported[kPNTriangesEXT ] = false;
	fExtensionSupported[kVertexShaderEXT] = false;
	fExtensionSupported[kFragmentShaderEXT] = false;


	// Initialize required extension(s)
	//

	// Initialize PN-triangles
	fExtensionSupported[kPNTriangesEXT] = initialize_ATI_Extension("GL_ATI_pn_triangles");
	// Get the maximum tessellation level supported by the extension
	//
	if (fExtensionSupported[kPNTriangesEXT])
	{
		int val;
		glGetIntegerv( GL_MAX_PN_TRIANGLES_TESSELATION_LEVEL_ATI, &val );
		fMaxTessellationLevel = val;
	}

	fExtensionSupported[kVertexShaderEXT] = initialize_ATI_Extension("GL_EXT_vertex_shader");
	fExtensionSupported[kFragmentShaderEXT] = initialize_ATI_Extension("GL_ATI_fragment_shader");
}

pnTriangles::~pnTriangles() 
//
// Description:
//		Destructor 
{
	// Delete any vertex shader
	//
	if (fExtensionSupported[kVertexShaderEXT])
	{
		if (fVertexShaderId != -1)
			glDeleteVertexShaderEXT(fVertexShaderId);
	}
}

bool pnTriangles::getInternalValue( const MPlug &plug, MDataHandle &dh ) 
//
// Description:
//		Get internally cached values when attribute is queried
//
{
    if( plug == pnTriangles::attrSubdivisions ) 
	{
		dh.set( int(fSubdivisions) );
		return true;
    }
	else if ( plug == pnTriangles::attrColored)
	{
		dh.set( (fNumColors > 0) );
		return true;
	}
	else if ( plug == pnTriangles::attrWireframe)
	{
		dh.set( fWireframe );
		return true;
	}
	else if ( plug == pnTriangles::attrTextured)
	{
		dh.set( (fNumTextureCoords > 0) );
		return true;
	}
	else if ( plug == pnTriangles::attrNormalMode )
	{
		if (fNormalMode == kNormalLinear)
			dh.set( 0 );
		else
			dh.set( 1 );
		return true;
	}
	else if ( plug == pnTriangles::attrPointMode )
	{
		if (fPointMode == kPointLinear)
			dh.set( 0 );
		else
			dh.set( 1 );
		return true;
	}
	else if ( plug == pnTriangles::attrEnableVertexProgram)
	{
		dh.set( fTestVertexProgram );
		return true;
	}
	else if ( plug == pnTriangles::attrEnablePixelProgram)
	{
		dh.set( fTestFragmentProgram );
		return true;
	}
	return false;
}

bool pnTriangles::setInternalValue( const MPlug &plug, const MDataHandle &dh ) 
//
// Description:
//		Set internally cached values when attribute changes
//
{
   if( plug == pnTriangles::attrSubdivisions ) 
	{
		fSubdivisions = dh.asInt();

		// Make sure we don't go above the maximum allowed by 
		// the extension
		if (fSubdivisions > fMaxTessellationLevel)
			fSubdivisions = fMaxTessellationLevel;
		return true;
    }
	else if ( plug == pnTriangles::attrColored)
	{
		if (dh.asBool() == true)
			fNumColors = 1;
		else
			fNumColors = 0;
		return true;
	}
	else if ( plug == pnTriangles::attrWireframe)
	{
		if (dh.asBool() == true)
			fWireframe = 1;
		else
			fWireframe = 0;
		return true;
	}
	else if ( plug == pnTriangles::attrTextured)
	{
		if (dh.asBool() == true)
			fNumTextureCoords = 1;
		else
			fNumTextureCoords = 0;
		return true;
	}
	else if ( plug == pnTriangles::attrNormalMode )
	{
		if (dh.asInt() == 0)
			fNormalMode = kNormalLinear;
		else
			fNormalMode = kNormalQuadratic;
		return true;
	}
	else if ( plug == pnTriangles::attrPointMode )
	{
		if (dh.asInt() == 0)
			fPointMode = kPointLinear;
		else
			fPointMode = kPointCubic;
		return true;
	}
	else if ( plug == pnTriangles::attrEnableVertexProgram )
	{
		if (dh.asBool() == true)
		{
			if (fExtensionSupported[kVertexShaderEXT])
				fTestVertexProgram = 1;
			else
				fTestVertexProgram = 0;
		}
		else
			fTestVertexProgram = 0;

		return true;
	}
	else if ( plug == pnTriangles::attrEnablePixelProgram )
	{
#ifdef _FRAGMENT_PROGRAM_READY_
		if (dh.asBool() == true)
			if (fExtensionSupported[kFragmentShaderEXT])
				fTestFragmentProgram = 1;
			else
				fTestFragmentProgram = 0;
		else
			fTestFragmentProgram = 0;
#else
		fTestFragmentProgram = 0;
#endif
		return true;
	}
    return false;
}

void* pnTriangles::creator()
//
//	Description:
//		Create new pnTriangles class
//
//	Return Value:
//		a new object of this type
//
{
	return new pnTriangles();
}

MStatus pnTriangles::initialize()
//
//	Description:
//		Initialize class
//	Return Values:
//		MS::kSuccess
//		MS::kFailure
//		
{
	MFnNumericAttribute nAttr;
	MStatus				stat;

	// Subivision attribute
	attrSubdivisions = nAttr.create( "subdivisions", "sd", MFnNumericData::kInt, 1 );
 	nAttr.setStorable(true);
	nAttr.setInternal(true);
	nAttr.setMin(0);
    nAttr.setMax(10);
    nAttr.setDefault(1);
	nAttr.setKeyable(true);

	attrColored = nAttr.create( "colored", "cl", MFnNumericData::kBoolean, 1 );
 	nAttr.setStorable(true);
	nAttr.setInternal(true);
    nAttr.setDefault( false );
	nAttr.setKeyable(true);

	attrTextured = nAttr.create( "textured", "tx", MFnNumericData::kBoolean, 1 );
 	nAttr.setStorable(true);
	nAttr.setInternal(true);
    nAttr.setDefault( false );
	nAttr.setKeyable(true);

	attrWireframe = nAttr.create( "wireframe", "wf", MFnNumericData::kBoolean, 1 );
 	nAttr.setStorable(true);
	nAttr.setInternal(true);
    nAttr.setDefault( false );
	nAttr.setKeyable(true);
	
	// Normal mode attribute
	attrNormalMode = nAttr.create( "quadraticNormals", "qn", MFnNumericData::kInt, 1 );
 	nAttr.setStorable(true);
	nAttr.setInternal(true);
	nAttr.setMin(0); // Linear
    nAttr.setMax(1); // Quadratic
    nAttr.setDefault(1);
	nAttr.setKeyable(true);

	// Point mode attribute
	attrPointMode= nAttr.create( "cubicPoints", "cp", MFnNumericData::kInt, 1 );
 	nAttr.setStorable(true);
	nAttr.setInternal(true);
	nAttr.setMin(0); // Linear
    nAttr.setMax(1); // Cubic
    nAttr.setDefault(1);
	nAttr.setKeyable(true);

	// Enable vertex program
	attrEnableVertexProgram = nAttr.create( "vertexProgram", "vp", MFnNumericData::kBoolean, 1 );
 	nAttr.setStorable(true);
	nAttr.setInternal(true);
    nAttr.setDefault( false );
	nAttr.setKeyable(true);
		
	// Enable pixel program
	attrEnablePixelProgram = nAttr.create( "pixelProgram", "pp", MFnNumericData::kBoolean, 1 );
 	nAttr.setStorable(true);
	nAttr.setInternal(true);
    nAttr.setDefault( false );
	nAttr.setKeyable(true);

	// Add the attributes we have created to the node
	//
	stat = addAttribute( attrSubdivisions );
	if (!stat) { stat.perror("addAttribute"); return stat;}
	stat = addAttribute( attrColored );
	if (!stat) { stat.perror("addAttribute"); return stat;}
	stat = addAttribute( attrWireframe );
	if (!stat) { stat.perror("addAttribute"); return stat;}
	stat = addAttribute( attrTextured );
	if (!stat) { stat.perror("addAttribute"); return stat;}
	stat = addAttribute( attrNormalMode );
	if (!stat) { stat.perror("addAttribute"); return stat;}
	stat = addAttribute( attrPointMode );
	if (!stat) { stat.perror("addAttribute"); return stat;}
	stat = addAttribute( attrEnableVertexProgram );
	if (!stat) { stat.perror("addAttribute"); return stat;}
	stat = addAttribute( attrEnablePixelProgram );
	if (!stat) { stat.perror("addAttribute"); return stat;}

	return MS::kSuccess;
}

MStatus pnTriangles::getFloat3(MObject attr, float value[3])
{
	MStatus status;
	// Get the attr to use
	//
	MPlug	plug(thisMObject(), attr);

	MObject object;

	status = plug.getValue(object);
	if (!status)
	{
		status.perror("pnTrianglesNode::bind plug.getValue.");
		return status;
	}


	MFnNumericData data(object, &status);
	if (!status)
	{
		status.perror("pnTrianglesNode::bind construct data.");
		return status;
	}

	status = data.getData(value[0], value[1], value[2]);
	if (!status)
	{
		status.perror("pnTrianglesNode::bind get values.");
		return status;
	}

	return MS::kSuccess;
}

#define DEG_TO_RAD(_x_) (3.14159265358979323846264338327950F / 180.0F * (_x_))

//////////////////////

void pnTriangles::bindVertexProgram(const MColor diffuse, 
									const MColor specular, 
									const MColor emission, 
									const MColor ambient)
//
// Description:
//		Sample vertex program which sets up a yellow diffuse
//		shader.
//									
{
	// See if extension is supported
	//
	if (!fExtensionSupported[kVertexShaderEXT])
		return;

	// Enable vertex shaders
	glEnable(GL_VERTEX_SHADER_EXT);

	// Generate a new vertex shader id, if none has been created.
	if (fVertexShaderId != -1)
	{
		glBindVertexShaderEXT (fVertexShaderId); 
		return;
	}

	fVertexShaderId = glGenVertexShadersEXT (1);

	//Initialize global parameter bindings
	GLuint Modelview = glBindParameterEXT (GL_MODELVIEW_MATRIX);
	GLuint Projection = glBindParameterEXT (GL_PROJECTION_MATRIX);
	// Q: does this read properly from vertex arrays or
	// do I need to define a variant pointer via glVariantPointerEXT ?
	//
	GLuint Vertex = glBindParameterEXT (GL_CURRENT_VERTEX_EXT);
	// GL_CURRENT_NORMAL comes from the glGetPointerTarget enums
	GLuint Normal = glBindParameterEXT (GL_CURRENT_NORMAL); 
	
	//GLuint lightPos = glBindLightParameterEXT( 0, GL_POSITION );

	// Bind a diffuse shader 
	glBindVertexShaderEXT (fVertexShaderId); 
	glBeginVertexShaderEXT ();
	{
		float direction[4] = { 0.57735f, 0.57735f, 0.57735f, 0.0f}; //direction vector (1,1,1) normalized
		GLuint lightDirection;
		GLuint diffMaterial;
		GLuint sceneAmbient;
		GLuint eyeVertex;
		GLuint clipVertex;
		GLuint eyeNormal;
		GLuint intensity;

		// generate local values
		eyeVertex = glGenSymbolsEXT (GL_VECTOR_EXT, GL_LOCAL_EXT, GL_FULL_RANGE_EXT, 1);
		clipVertex = glGenSymbolsEXT (GL_VECTOR_EXT, GL_LOCAL_EXT, GL_FULL_RANGE_EXT, 1);
		eyeNormal = glGenSymbolsEXT (GL_VECTOR_EXT, GL_LOCAL_EXT, GL_FULL_RANGE_EXT, 1);
		intensity = glGenSymbolsEXT (GL_VECTOR_EXT, GL_LOCAL_EXT, GL_FULL_RANGE_EXT, 1);

		// generate constant values
		lightDirection = glGenSymbolsEXT (GL_VECTOR_EXT, GL_LOCAL_CONSTANT_EXT, GL_FULL_RANGE_EXT, 1);
		diffMaterial = glGenSymbolsEXT (GL_VECTOR_EXT, GL_LOCAL_CONSTANT_EXT, GL_FULL_RANGE_EXT, 1);
		sceneAmbient = glGenSymbolsEXT (GL_VECTOR_EXT, GL_LOCAL_CONSTANT_EXT, GL_FULL_RANGE_EXT, 1);
		glSetLocalConstantEXT (lightDirection, GL_FLOAT, direction);
		glSetLocalConstantEXT (diffMaterial, GL_FLOAT, (void *)&(diffuse.r));
		glSetLocalConstantEXT (sceneAmbient, GL_FLOAT, (void *)&(ambient.r));
		glShaderOp2EXT (GL_OP_MULTIPLY_MATRIX_EXT, eyeVertex, Modelview, Vertex);
		glShaderOp2EXT (GL_OP_MULTIPLY_MATRIX_EXT, GL_OUTPUT_VERTEX_EXT, Projection, eyeVertex);

		// assumes no scaling/shearing in modelview matrix
		glShaderOp2EXT (GL_OP_MULTIPLY_MATRIX_EXT, eyeNormal, Modelview, Normal);
		glShaderOp2EXT (GL_OP_DOT3_EXT, intensity, lightDirection, eyeNormal);
		glShaderOp2EXT (GL_OP_ADD_EXT, intensity, sceneAmbient, intensity);
		glShaderOp2EXT (GL_OP_MUL_EXT, GL_OUTPUT_COLOR0_EXT, diffMaterial, intensity);
	}
	glEndVertexShaderEXT ();

}

void pnTriangles::bindFragmentProgram()
//
// Description:
//
//	THIS IS JUST SAMPLE CODE, MORE NEEDS TO BE DONE TO ACTUALLY USE
//	INCORPORATE A FRAGMENT SHADER.
//
{
	// See if extension is supported
	//
	if (!fExtensionSupported[kFragmentShaderEXT])
		return;

	// Enable vertex shaders
	glEnable(GL_FRAGMENT_SHADER_ATI);

	// Generate a new fragment shader id, if none has been created.
	if (fFragmentShaderId != -1)
	{
		glBindFragmentShaderATI (fFragmentShaderId);
		return;
	}

	fFragmentShaderId = glGenFragmentShadersATI(1);

	glBindFragmentShaderATI (fFragmentShaderId);
	glBeginFragmentShaderATI();

	// Place sample fragment shader code here ....

	glEndFragmentShaderATI();
}

/* virtual */
MStatus	pnTriangles::bind(const MDrawRequest& request, M3dView& view)
//
// Description:
//		This bind demonstrates the usage of internal material
//		and texture properties. This shader must be connected
//		to the "hardwareShader" attribute of a lambert derived
//		shader.
//
{
	// Setup the view
	view.beginGL();
	glPushAttrib( GL_ALL_ATTRIB_BITS );
	glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);

	MColor diffuse(1.0F, 1.0F, 0.0F, 1.0F);
	MColor specular(1.0F, 1.0F, 1.0F, 1.0F);
	MColor emission(0.0F, 0.0F, 0.0F, 1.0F);
	MColor ambient(0.2F, 0.2F, 0.2F, 1.0F);

	// Get the diffuse and specular colors
	//
	float shininess;
	bool hasTransparency = false;

	MMaterial material = request.material();
	fInTexturedMode = material.materialIsTextured();

	// Setting this to true will get the default "green" material back
	// since it will try and evaluate this shader, which internally
	// Maya does not understand -> thus giving the "green" material back
	bool useInternalMaterialSetting = false;
	
	if (!useInternalMaterialSetting)
	{
		material.getEmission( emission );
		material.getSpecular( specular );
		shininess = 13.0;
	}
	material.getHasTransparency( hasTransparency );	

	if (!fInTexturedMode)
	{
		if (!fTestVertexProgram && !useInternalMaterialSetting)
			material.getDiffuse( diffuse );
	}
	// In textured mode. Diffuse material is always white
	// for texture blends
	else
	{
		if (!useInternalMaterialSetting)
			diffuse.r = diffuse.g = diffuse.b = diffuse.a = 1.0;
	}

	// Use a vertex program to set up shading
	//
	if (fTestVertexProgram)
	{
		bindVertexProgram(diffuse, specular, emission, ambient);
	}
	else if (fTestFragmentProgram)
	{
		bindFragmentProgram();
	}

	// Don't use a vertex program to set up shading
	//
	else
	{
		// Set up the material state
		//
		glEnable(GL_COLOR_MATERIAL);
		glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT);
		glColor4fv(&ambient.r);

		if (fInTexturedMode)
		{
			glEnable( GL_TEXTURE_2D );
			MDrawData drawData = request.drawData();
			material.applyTexture( view, drawData );

			float scaleS, scaleT, translateS, translateT, rotate;

			material.getTextureTransformation(scaleS, scaleT, translateS,
											  translateT, rotate);

			rotate = DEG_TO_RAD(-rotate);
			float c = cosf(rotate);
			float s = sinf(rotate);
			translateS += ((c+s)/2.0F);
			translateT += ((c-s)/2.0F);

			glMatrixMode(GL_TEXTURE);
			glPushMatrix();
			glLoadIdentity();

			if(scaleS != 1.0f || scaleT != 1.0f)
				glScalef(1.0f/scaleS, 1.0f/scaleT, 1.0f);
			if(translateS != 0.0f || translateT != 0.0f)
				glTranslatef(0.5f-translateS, 0.5f-translateT, 0.0f);
			else
				glTranslatef(0.5f, 0.5f, 0.0f);

			if(rotate != 0.0f)
				glRotatef(-rotate, 0.0f, 0.0f, 1.0f);

			glMatrixMode(GL_MODELVIEW);
		}

		if (!useInternalMaterialSetting)
		{
			glColorMaterial(GL_FRONT_AND_BACK, GL_DIFFUSE);
			glColor4fv(&diffuse.r);
			glColorMaterial(GL_FRONT_AND_BACK, GL_SPECULAR);
			glColor4fv(&specular.r);
			glColorMaterial(GL_FRONT_AND_BACK, GL_EMISSION);
			glColor4fv(&emission.r);
			glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, shininess);
		}
		else
		{
			const MDagPath dagPath =  request.multiPath();
			material.evaluateMaterial( view, dagPath );
			material.setMaterial(dagPath, hasTransparency);
		}
	}

	// Do PN triangles in hardware, or do nothing
	// if LOD = 0
	if (fExtensionSupported[kPNTriangesEXT] || (fSubdivisions == 0))
	{
		if (fSubdivisions != 0)
		{
			glEnable( GL_PN_TRIANGLES_ATI );

			// Set point mode
			//
			if (fPointMode == kPointLinear)
				glPNTrianglesiATI( GL_PN_TRIANGLES_POINT_MODE_ATI, 
								   GL_PN_TRIANGLES_POINT_MODE_LINEAR_ATI);
			else
				glPNTrianglesiATI( GL_PN_TRIANGLES_POINT_MODE_ATI, 
								   GL_PN_TRIANGLES_POINT_MODE_CUBIC_ATI);

			// Set normal mode
			//
			if (fNormalMode == kNormalLinear)
				glPNTrianglesiATI( GL_PN_TRIANGLES_NORMAL_MODE_ATI, 
								   GL_PN_TRIANGLES_NORMAL_MODE_LINEAR_ATI);
			else
				glPNTrianglesiATI( GL_PN_TRIANGLES_NORMAL_MODE_ATI, 
								   GL_PN_TRIANGLES_NORMAL_MODE_QUADRATIC_ATI);

			// Set tessellation level
			//
			glPNTrianglesiATI( GL_PN_TRIANGLES_TESSELATION_LEVEL_ATI,
							  fSubdivisions );
		}
	}

	view.endGL();

	return MS::kSuccess;
}

/* virtual */
MStatus	pnTriangles::unbind(const MDrawRequest& request,
						    M3dView& view)
//
// Description:
//		
//
{
	view.beginGL();

	// Disable vertex shader
	if (fTestVertexProgram)
	{
		glDisable(GL_VERTEX_SHADER_EXT);
	}
	// Disable fragment shader
	else if (fTestFragmentProgram)
	{
		glDisable(GL_FRAGMENT_SHADER_ATI);
	}
	// Disable regular material state
	else
	{
		glDisable( GL_TEXTURE_2D );
		glDisable(GL_COLOR_MATERIAL);

		if (fInTexturedMode)
		{
			glMatrixMode(GL_TEXTURE);
			glPopMatrix();
			glMatrixMode(GL_MODELVIEW);
		}
	}

	// Disable PN triangles extension if needed
	//
	if (fExtensionSupported[kPNTriangesEXT] || (fSubdivisions == 0))
	{
		if (fSubdivisions != 0)
			glDisable( GL_PN_TRIANGLES_ATI );
	}
	
	glPopClientAttrib();
	glPopAttrib();

	view.endGL();

	return MS::kSuccess;
}

unsigned int pnTriangles:: computePNTriangles(const float * vertexArray,
									  const float * normalArray,
									  const float * texCoordArray,
									  float * pnVertexArray,
									  float * pnNormalArray,
									  float * pnTexCoordArray,
									  float * pnColorArray)
{
	unsigned int triangleCount = 0;
	// Software implementation left to the user...
	return triangleCount;
}

/* virtual */
MStatus	pnTriangles::geometry( const MDrawRequest& request,
								M3dView& view,
							    int prim,
								unsigned int writable,
								int indexCount,
								const unsigned int * indexArray,
								int vertexCount,
								const int * vertexIDs,
								const float * vertexArray,
								int normalCount,
								const float ** normalArrays,
								int colorCount,
								const float ** colorArrays,
								int texCoordCount,
								const float ** texCoordArrays)
{
	// We assume triangles here.
	//
	if (prim != GL_TRIANGLES)
		return MS::kSuccess;	// Should call parent class here !

	view.beginGL();

	if (fWireframe)
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

	bool needColors = (fNumColors && colorCount);
	bool needTexCoords = (fNumTextureCoords && texCoordCount);
	bool needNormals = (fNumNormals && normalCount);

	if (!fExtensionSupported[kPNTriangesEXT])
	{
		// Write code here to support PN-triangles in software...
	}

	glVertexPointer(3, GL_FLOAT, 0, vertexArray);
	glEnableClientState(GL_VERTEX_ARRAY);

	if (needNormals)
	{
		glNormalPointer(GL_FLOAT, 0, normalArrays[0]);
		glEnableClientState(GL_NORMAL_ARRAY);
	}
	if (needTexCoords)
	{
		glTexCoordPointer(2, GL_FLOAT, 0, texCoordArrays[0]);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	}
	if (needColors)
	{
		// Override material color
		glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
		glColorPointer(4, GL_FLOAT, 0, colorArrays[0]);
		glEnableClientState(GL_COLOR_ARRAY);
	}

	glDrawElements(prim, indexCount, GL_UNSIGNED_INT, indexArray);

	if (fWireframe)
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

	view.endGL();


	return MS::kSuccess;
}

/* virtual */
int	pnTriangles::normalsPerVertex()
{
	return fNumNormals;
}

/* virtual */
int	pnTriangles::texCoordsPerVertex()
{
	if (fInTexturedMode)
		return fNumTextureCoords;
	else
		return 0;
}

/* virtual */
int	pnTriangles::colorsPerVertex()
{
	return fNumColors;
}

//////////////////////////////////////////////////////////////////////////////////////////////
// Main plugin interface
//////////////////////////////////////////////////////////////////////////////////////////////
MStatus initializePlugin( MObject obj )
//
//	Description:
//		Initialize plugin
//
//	Arguments:
//		obj - a handle to the plug-in object (use MFnPlugin to access it)
//
{ 
	MStatus   status;

	const MString UserClassify( "shader/surface/utility" );
	
	MFnPlugin plugin( obj, PLUGIN_COMPANY, "4.5", "Any");

	status = plugin.registerNode( "pnTriangles", pnTriangles::id, pnTriangles::creator,
								  pnTriangles::initialize, MPxNode::kHwShaderNode, &UserClassify  );
	if (!status) {
		status.perror("registerNode");
		return status;
	}

	return status;
}

MStatus uninitializePlugin( MObject obj)
//
//	Description:
//		Deregister node
//
//	Arguments:
//		obj - a handle to the plug-in object (use MFnPlugin to access it)
//
{
	MStatus   status;
	MFnPlugin plugin( obj );

	status = plugin.deregisterNode( pnTriangles::id );
	if (!status) {
		status.perror("deregisterNode");
		return status;
	}

	return status;
}
