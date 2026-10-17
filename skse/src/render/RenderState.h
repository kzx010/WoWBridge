#pragma once

#include <d3d11.h>

// Exact bindings for the slots our world/menu draws touch; restore before returning to the engine's state cache.
namespace RenderState
{
	template <class T>
	void Release(T*& a_p)
	{
		if (a_p) { a_p->Release(); a_p = nullptr; }
	}
	struct StateBackup
	{
		ID3D11RenderTargetView*  rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView*  dsv = nullptr;
		ID3D11BlendState*        blend = nullptr;
		float                    factor[4]{};
		UINT                     mask = 0;
		ID3D11RasterizerState*   raster = nullptr;
		ID3D11DepthStencilState* depth = nullptr;
		UINT                     stencil = 0;
		D3D11_VIEWPORT           vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		UINT                     vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		D3D11_PRIMITIVE_TOPOLOGY topo{};
		ID3D11InputLayout*       layout = nullptr;
		ID3D11Buffer*            vb = nullptr;
		UINT                     stride = 0, offset = 0;
		ID3D11Buffer*            ib = nullptr;
		DXGI_FORMAT              ibFormat{};
		UINT                     ibOffset = 0;
		ID3D11VertexShader*      vs = nullptr;
		ID3D11PixelShader*       ps = nullptr;
		ID3D11GeometryShader*    gs = nullptr;
		ID3D11HullShader*        hs = nullptr;
		ID3D11DomainShader*      ds = nullptr;
		ID3D11ClassInstance*     vsClasses[256]{}, *psClasses[256]{}, *gsClasses[256]{}, *hsClasses[256]{}, *dsClasses[256]{};
		UINT                     vsCount = 256, psCount = 256, gsCount = 256, hsCount = 256, dsCount = 256;
		D3D11_RECT               scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		UINT                     scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		ID3D11Predicate*         predicate = nullptr;
		BOOL                     predicateValue = FALSE;
		ID3D11Buffer*            vsCb[3]{};
		ID3D11Buffer*            psCb[4]{};
		ID3D11ShaderResourceView* psSrv[4]{};
		ID3D11SamplerState*      psSampler[2]{};

		void Save(ID3D11DeviceContext* c)
		{
			c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
			c->OMGetBlendState(&blend, factor, &mask);
			c->RSGetState(&raster);
			c->OMGetDepthStencilState(&depth, &stencil);
			c->RSGetViewports(&vpCount, vps);
			c->RSGetScissorRects(&scissorCount, scissors);
			c->GetPredication(&predicate, &predicateValue);
			c->IAGetPrimitiveTopology(&topo);
			c->IAGetInputLayout(&layout);
			c->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
			c->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);
			c->VSGetShader(&vs, vsClasses, &vsCount);
			c->PSGetShader(&ps, psClasses, &psCount);
			c->GSGetShader(&gs, gsClasses, &gsCount);
			c->HSGetShader(&hs, hsClasses, &hsCount);
			c->DSGetShader(&ds, dsClasses, &dsCount);
			c->VSGetConstantBuffers(0, 3, vsCb);
			c->PSGetConstantBuffers(0, 4, psCb);
			c->PSGetShaderResources(0, 4, psSrv);
			c->PSGetSamplers(0, 2, psSampler);
		}

		void Restore(ID3D11DeviceContext* c)
		{
			c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
			c->OMSetBlendState(blend, factor, mask);
			c->RSSetState(raster);
			c->OMSetDepthStencilState(depth, stencil);
			c->RSSetViewports(vpCount, vps);
			c->RSSetScissorRects(scissorCount, scissors);
			c->SetPredication(predicate, predicateValue);
			c->IASetPrimitiveTopology(topo);
			c->IASetInputLayout(layout);
			c->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
			c->IASetIndexBuffer(ib, ibFormat, ibOffset);
			c->VSSetShader(vs, vsClasses, vsCount);
			c->PSSetShader(ps, psClasses, psCount);
			c->GSSetShader(gs, gsClasses, gsCount);
			c->HSSetShader(hs, hsClasses, hsCount);
			c->DSSetShader(ds, dsClasses, dsCount);
			c->VSSetConstantBuffers(0, 3, vsCb);
			c->PSSetConstantBuffers(0, 4, psCb);
			c->PSSetShaderResources(0, 4, psSrv);
			c->PSSetSamplers(0, 2, psSampler);
			for (auto*& srv : psSrv) { Release(srv); }
			for (auto* instances : { vsClasses, psClasses, gsClasses, hsClasses, dsClasses }) {
				for (UINT i = 0; i < 256; ++i) { Release(instances[i]); }
			}
			Release(gs), Release(hs), Release(ds), Release(predicate);
			for (auto*& sampler : psSampler) { Release(sampler); }
			for (auto*& r : rtv) {
				Release(r);
			}
			for (auto*& b : vsCb) {
				Release(b);
			}
			for (auto*& b : psCb) {
				Release(b);
			}
			Release(dsv), Release(blend), Release(raster), Release(depth), Release(layout), Release(vb), Release(ib), Release(vs), Release(ps);
		}
	};
}
