//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Real D3D9 device/texture/mesh calls come from libdxvk_d3d9.so, but D3DX9
// (matrix/vector math, the matrix stack, and the HLSL shader compiler) was
// historically a *separate* utility library on Windows (d3dx9_43.dll etc,
// distributed with the DirectX SDK) - DXVK Native doesn't reimplement it,
// since it isn't part of the D3D9 device/runtime API surface DXVK targets.
// This provides the small subset of D3DX9 that materialsystem/shaderapidx9's
// code actually calls: matrix/vector/plane math (implemented directly,
// following the standard D3D9 row-vector formulas) and ID3DXMatrixStack
// (a straightforward stack of D3DXMATRIX). D3DXCompileShader/
// D3DXGetShaderVersion are stubbed to fail - Source ships shaders as
// precompiled bytecode (.vcs) normally, dynamic HLSL compilation is a
// separate dev-only path this doesn't need to support.
//
//===========================================================================//

#include "togl/rendermechanism.h"
#include <math.h>
#include <string.h>

//-----------------------------------------------------------------------------
// Matrix / vector / plane math
//-----------------------------------------------------------------------------

D3DXMATRIX* WINAPI D3DXMatrixMultiply( D3DXMATRIX *pout, const D3DXMATRIX *pm1, const D3DXMATRIX *pm2 )
{
	D3DXMATRIX result;
	for ( int i = 0; i < 4; ++i )
	{
		for ( int j = 0; j < 4; ++j )
		{
			result.m[i][j] = pm1->m[i][0] * pm2->m[0][j]
			                + pm1->m[i][1] * pm2->m[1][j]
			                + pm1->m[i][2] * pm2->m[2][j]
			                + pm1->m[i][3] * pm2->m[3][j];
		}
	}
	*pout = result;
	return pout;
}

D3DXMATRIX* WINAPI D3DXMatrixTranspose( D3DXMATRIX *pout, const D3DXMATRIX *pm )
{
	D3DXMATRIX result;
	for ( int i = 0; i < 4; ++i )
		for ( int j = 0; j < 4; ++j )
			result.m[i][j] = pm->m[j][i];
	*pout = result;
	return pout;
}

D3DXMATRIX* WINAPI D3DXMatrixTranslation( D3DXMATRIX *pout, FLOAT x, FLOAT y, FLOAT z )
{
	D3DXMatrixIdentity( pout );
	pout->_41 = x;
	pout->_42 = y;
	pout->_43 = z;
	return pout;
}

D3DXMATRIX* WINAPI D3DXMatrixInverse( D3DXMATRIX *pout, FLOAT *pdeterminant, const D3DXMATRIX *pm )
{
	const FLOAT *m = &pm->_11;
	FLOAT inv[16];

	inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
	inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
	inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
	inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];

	inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
	inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
	inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
	inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];

	inv[2]  =  m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
	inv[6]  = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
	inv[10] =  m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
	inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];

	inv[3]  = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
	inv[7]  =  m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
	inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9]  + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
	inv[15] =  m[0]*m[5]*m[10] - m[0]*m[6]*m[9]  - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

	FLOAT det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];

	if ( pdeterminant )
		*pdeterminant = det;

	if ( det == 0.0f )
		return NULL;

	FLOAT invDet = 1.0f / det;
	FLOAT *out = &pout->_11;
	for ( int i = 0; i < 16; ++i )
		out[i] = inv[i] * invDet;

	return pout;
}

D3DXMATRIX* WINAPI D3DXMatrixOrthoOffCenterRH( D3DXMATRIX *pout, FLOAT l, FLOAT r, FLOAT b, FLOAT t, FLOAT zn, FLOAT zf )
{
	D3DXMatrixIdentity( pout );
	pout->_11 = 2.0f / (r - l);
	pout->_22 = 2.0f / (t - b);
	pout->_33 = 1.0f / (zn - zf);
	pout->_41 = (l + r) / (l - r);
	pout->_42 = (t + b) / (b - t);
	pout->_43 = zn / (zn - zf);
	return pout;
}

D3DXMATRIX* WINAPI D3DXMatrixPerspectiveRH( D3DXMATRIX *pout, FLOAT w, FLOAT h, FLOAT zn, FLOAT zf )
{
	memset( pout, 0, sizeof( D3DXMATRIX ) );
	pout->_11 = 2.0f * zn / w;
	pout->_22 = 2.0f * zn / h;
	pout->_33 = zf / (zn - zf);
	pout->_34 = -1.0f;
	pout->_43 = zn * zf / (zn - zf);
	return pout;
}

D3DXMATRIX* WINAPI D3DXMatrixPerspectiveOffCenterRH( D3DXMATRIX *pout, FLOAT l, FLOAT r, FLOAT b, FLOAT t, FLOAT zn, FLOAT zf )
{
	memset( pout, 0, sizeof( D3DXMATRIX ) );
	pout->_11 = 2.0f * zn / (r - l);
	pout->_22 = 2.0f * zn / (t - b);
	pout->_31 = (l + r) / (r - l);
	pout->_32 = (t + b) / (t - b);
	pout->_33 = zf / (zn - zf);
	pout->_34 = -1.0f;
	pout->_43 = zn * zf / (zn - zf);
	return pout;
}

D3DXVECTOR3* WINAPI D3DXVec3TransformCoord( D3DXVECTOR3 *pout, const D3DXVECTOR3 *pv, const D3DXMATRIX *pm )
{
	FLOAT x = pv->x * pm->_11 + pv->y * pm->_21 + pv->z * pm->_31 + pm->_41;
	FLOAT y = pv->x * pm->_12 + pv->y * pm->_22 + pv->z * pm->_32 + pm->_42;
	FLOAT z = pv->x * pm->_13 + pv->y * pm->_23 + pv->z * pm->_33 + pm->_43;
	FLOAT w = pv->x * pm->_14 + pv->y * pm->_24 + pv->z * pm->_34 + pm->_44;

	if ( w == 0.0f )
		w = 1.0f;

	pout->x = x / w;
	pout->y = y / w;
	pout->z = z / w;
	return pout;
}

D3DXVECTOR4* WINAPI D3DXVec4Transform( D3DXVECTOR4 *pout, const D3DXVECTOR4 *pv, const D3DXMATRIX *pm )
{
	FLOAT x = pv->x * pm->_11 + pv->y * pm->_21 + pv->z * pm->_31 + pv->w * pm->_41;
	FLOAT y = pv->x * pm->_12 + pv->y * pm->_22 + pv->z * pm->_32 + pv->w * pm->_42;
	FLOAT z = pv->x * pm->_13 + pv->y * pm->_23 + pv->z * pm->_33 + pv->w * pm->_43;
	FLOAT w = pv->x * pm->_14 + pv->y * pm->_24 + pv->z * pm->_34 + pv->w * pm->_44;

	pout->x = x;
	pout->y = y;
	pout->z = z;
	pout->w = w;
	return pout;
}

D3DXVECTOR4* WINAPI D3DXVec4Normalize( D3DXVECTOR4 *pout, const D3DXVECTOR4 *pv )
{
	FLOAT len = sqrtf( pv->x*pv->x + pv->y*pv->y + pv->z*pv->z + pv->w*pv->w );
	if ( len == 0.0f )
	{
		pout->x = pout->y = pout->z = pout->w = 0.0f;
		return pout;
	}
	pout->x = pv->x / len;
	pout->y = pv->y / len;
	pout->z = pv->z / len;
	pout->w = pv->w / len;
	return pout;
}

D3DXPLANE* WINAPI D3DXPlaneNormalize( D3DXPLANE *pout, const D3DXPLANE *pp )
{
	FLOAT len = sqrtf( pp->a*pp->a + pp->b*pp->b + pp->c*pp->c );
	if ( len == 0.0f )
	{
		*pout = *pp;
		return pout;
	}
	pout->a = pp->a / len;
	pout->b = pp->b / len;
	pout->c = pp->c / len;
	pout->d = pp->d / len;
	return pout;
}

D3DXPLANE* WINAPI D3DXPlaneTransform( D3DXPLANE *pout, const D3DXPLANE *pplane, const D3DXMATRIX *pm )
{
	// Plane normals must be transformed by the inverse-transpose to remain
	// correct under non-uniform scale - standard technique, matches D3DX9's
	// own documented/reference behavior.
	D3DXMATRIX inv, invT;
	D3DXMatrixInverse( &inv, NULL, pm );
	D3DXMatrixTranspose( &invT, &inv );

	FLOAT a = pplane->a, b = pplane->b, c = pplane->c, d = pplane->d;
	pout->a = a * invT._11 + b * invT._21 + c * invT._31 + d * invT._41;
	pout->b = a * invT._12 + b * invT._22 + c * invT._32 + d * invT._42;
	pout->c = a * invT._13 + b * invT._23 + c * invT._33 + d * invT._43;
	pout->d = a * invT._14 + b * invT._24 + c * invT._34 + d * invT._44;
	return pout;
}

//-----------------------------------------------------------------------------
// ID3DXMatrixStack - a plain stack of D3DXMATRIX. MultMatrix pre-multiplies
// ("world/global" order: new = pM * top), MultMatrixLocal post-multiplies
// ("local/object" order: new = top * pM) - matches D3DX9's documented
// semantics for these two variants.
//-----------------------------------------------------------------------------

static D3DXMATRIX* MatrixRotationAxis( D3DXMATRIX *pout, const D3DXVECTOR3 *pv, FLOAT angle )
{
	FLOAT x = pv->x, y = pv->y, z = pv->z;
	FLOAT len = sqrtf( x*x + y*y + z*z );
	if ( len > 0.0f )
	{
		x /= len; y /= len; z /= len;
	}

	FLOAT s = sinf( angle );
	FLOAT c = cosf( angle );
	FLOAT t = 1.0f - c;

	D3DXMatrixIdentity( pout );
	pout->_11 = t*x*x + c;
	pout->_12 = t*x*y + s*z;
	pout->_13 = t*x*z - s*y;
	pout->_21 = t*x*y - s*z;
	pout->_22 = t*y*y + c;
	pout->_23 = t*y*z + s*x;
	pout->_31 = t*x*z + s*y;
	pout->_32 = t*y*z - s*x;
	pout->_33 = t*z*z + c;
	return pout;
}

class CD3DXMatrixStackImpl : public ID3DXMatrixStack
{
public:
	CD3DXMatrixStackImpl() : m_nRefCount( 1 ), m_nTop( 0 )
	{
		D3DXMatrixIdentity( &m_Stack[0] );
	}

	// IUnknown
	STDMETHOD(QueryInterface)( REFIID riid, void **ppvObj )
	{
		*ppvObj = this;
		AddRef();
		return D3D_OK;
	}
	STDMETHOD_(ULONG,AddRef)()
	{
		return ++m_nRefCount;
	}
	STDMETHOD_(ULONG,Release)()
	{
		ULONG nRef = --m_nRefCount;
		if ( nRef == 0 )
			delete this;
		return nRef;
	}

	// ID3DXMatrixStack
	STDMETHOD(Pop)()
	{
		if ( m_nTop > 0 )
			--m_nTop;
		return D3D_OK;
	}
	STDMETHOD(Push)()
	{
		if ( m_nTop + 1 >= MAX_STACK_DEPTH )
			return E_OUTOFMEMORY;
		m_Stack[m_nTop + 1] = m_Stack[m_nTop];
		++m_nTop;
		return D3D_OK;
	}
	STDMETHOD(LoadIdentity)()
	{
		D3DXMatrixIdentity( &m_Stack[m_nTop] );
		return D3D_OK;
	}
	STDMETHOD(LoadMatrix)( const D3DXMATRIX *pM )
	{
		m_Stack[m_nTop] = *pM;
		return D3D_OK;
	}
	STDMETHOD(MultMatrix)( const D3DXMATRIX *pM )
	{
		D3DXMatrixMultiply( &m_Stack[m_nTop], pM, &m_Stack[m_nTop] );
		return D3D_OK;
	}
	STDMETHOD(MultMatrixLocal)( const D3DXMATRIX *pM )
	{
		D3DXMatrixMultiply( &m_Stack[m_nTop], &m_Stack[m_nTop], pM );
		return D3D_OK;
	}
	STDMETHOD(RotateAxis)( const D3DXVECTOR3 *pV, FLOAT Angle )
	{
		D3DXMATRIX rot;
		MatrixRotationAxis( &rot, pV, Angle );
		return MultMatrix( &rot );
	}
	STDMETHOD(RotateAxisLocal)( const D3DXVECTOR3 *pV, FLOAT Angle )
	{
		D3DXMATRIX rot;
		MatrixRotationAxis( &rot, pV, Angle );
		return MultMatrixLocal( &rot );
	}
	STDMETHOD(RotateYawPitchRoll)( FLOAT Yaw, FLOAT Pitch, FLOAT Roll )
	{
		D3DXMATRIX rot;
		BuildYawPitchRoll( &rot, Yaw, Pitch, Roll );
		return MultMatrix( &rot );
	}
	STDMETHOD(RotateYawPitchRollLocal)( FLOAT Yaw, FLOAT Pitch, FLOAT Roll )
	{
		D3DXMATRIX rot;
		BuildYawPitchRoll( &rot, Yaw, Pitch, Roll );
		return MultMatrixLocal( &rot );
	}
	STDMETHOD(Scale)( FLOAT x, FLOAT y, FLOAT z )
	{
		D3DXMATRIX scale;
		D3DXMatrixIdentity( &scale );
		scale._11 = x; scale._22 = y; scale._33 = z;
		return MultMatrix( &scale );
	}
	STDMETHOD(ScaleLocal)( FLOAT x, FLOAT y, FLOAT z )
	{
		D3DXMATRIX scale;
		D3DXMatrixIdentity( &scale );
		scale._11 = x; scale._22 = y; scale._33 = z;
		return MultMatrixLocal( &scale );
	}
	STDMETHOD(Translate)( FLOAT x, FLOAT y, FLOAT z )
	{
		D3DXMATRIX trans;
		D3DXMatrixTranslation( &trans, x, y, z );
		return MultMatrix( &trans );
	}
	STDMETHOD(TranslateLocal)( FLOAT x, FLOAT y, FLOAT z )
	{
		D3DXMATRIX trans;
		D3DXMatrixTranslation( &trans, x, y, z );
		return MultMatrixLocal( &trans );
	}
	STDMETHOD_(D3DXMATRIX*,GetTop)()
	{
		return &m_Stack[m_nTop];
	}

private:
	static void BuildYawPitchRoll( D3DXMATRIX *pout, FLOAT yaw, FLOAT pitch, FLOAT roll )
	{
		FLOAT sy = sinf(yaw),   cy = cosf(yaw);
		FLOAT sp = sinf(pitch), cp = cosf(pitch);
		FLOAT sr = sinf(roll),  cr = cosf(roll);

		D3DXMatrixIdentity( pout );
		pout->_11 = cr*cy + sr*sp*sy;
		pout->_12 = sr*cp;
		pout->_13 = cr*-sy + sr*sp*cy;
		pout->_21 = -sr*cy + cr*sp*sy;
		pout->_22 = cr*cp;
		pout->_23 = sr*sy + cr*sp*cy;
		pout->_31 = cp*sy;
		pout->_32 = -sp;
		pout->_33 = cp*cy;
	}

	static const int MAX_STACK_DEPTH = 32;
	ULONG m_nRefCount;
	int m_nTop;
	D3DXMATRIX m_Stack[MAX_STACK_DEPTH];
};

HRESULT WINAPI D3DXCreateMatrixStack( DWORD flags, ID3DXMatrixStack **ppStack )
{
	*ppStack = new CD3DXMatrixStackImpl();
	return D3D_OK;
}

//-----------------------------------------------------------------------------
// Dynamic HLSL compilation - not needed at runtime (shaders ship precompiled
// as .vcs bytecode); fail cleanly rather than vendoring a full HLSL compiler.
//-----------------------------------------------------------------------------

DWORD WINAPI D3DXGetShaderVersion( const DWORD *byte_code )
{
	return byte_code ? *byte_code : 0;
}

HRESULT WINAPI D3DXCompileShader( const char *src_data, UINT data_len, const D3DXMACRO *defines,
	ID3DXInclude *include, const char *function_name, const char *profile, DWORD flags,
	ID3DXBuffer **shader, ID3DXBuffer **error_messages, ID3DXConstantTable **constant_table )
{
	if ( shader )
		*shader = NULL;
	if ( error_messages )
		*error_messages = NULL;
	if ( constant_table )
		*constant_table = NULL;
	return E_NOTIMPL;
}
