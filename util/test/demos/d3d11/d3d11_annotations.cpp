/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2023-2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "d3d11_test.h"

RD_TEST(D3D11_Annotations, D3D11GraphicsTest)
{
  static constexpr const char *Description = "Test annotations via the D3D11 API.";

  int main()
  {
    // initialise, create window, create device, etc
    if(!Init())
      return 3;

    ID3D11Texture2DPtr img = MakeTexture(DXGI_FORMAT_R32G32B32A32_FLOAT, 4, 4).RTV();
    ID3D11RenderTargetViewPtr imgRTV = MakeRTV(img);

    SetDebugName(img, "Annotated Image");
    SetDebugName(DefaultTriVB, "Vertex Buffer");

    // cache the device pointer we pass in
    void *d = dev;

    if(rdoc)
    {
      rdoc->SetObjectAnnotation(d, img, "basic.bool", eNOOBDAWN_Bool, 0, RDAnnotationHelper(true));
      rdoc->SetObjectAnnotation(d, img, "basic.int32", eNOOBDAWN_Int32, 0, RDAnnotationHelper(-3));
      rdoc->SetObjectAnnotation(d, img, "basic.int64", eNOOBDAWN_Int64, 0,
                                RDAnnotationHelper(-3000000000000LL));
      rdoc->SetObjectAnnotation(d, img, "basic.uint32", eNOOBDAWN_UInt32, 0, RDAnnotationHelper(3));
      rdoc->SetObjectAnnotation(d, img, "basic.uint64", eNOOBDAWN_UInt64, 0,
                                RDAnnotationHelper(3000000000000LL));
      rdoc->SetObjectAnnotation(d, img, "basic.float", eNOOBDAWN_Float, 0,
                                RDAnnotationHelper(3.25f));
      rdoc->SetObjectAnnotation(d, img, "basic.double", eNOOBDAWN_Double, 0,
                                RDAnnotationHelper(3.25000000001));
      rdoc->SetObjectAnnotation(d, img, "basic.string", eNOOBDAWN_String, 0,
                                RDAnnotationHelper("Hello, World!"));

      NOOBDAWN_AnnotationValue val;
      val.apiObject = (void *)DefaultTriVB;
      rdoc->SetObjectAnnotation(d, img, "basic.object", eNOOBDAWN_APIObject, 0, &val);

      rdoc->SetObjectAnnotation(d, img, "basic.object.__offset", eNOOBDAWN_UInt32, 0,
                                RDAnnotationHelper(64));
      rdoc->SetObjectAnnotation(d, img, "basic.object.__size", eNOOBDAWN_UInt32, 0,
                                RDAnnotationHelper(32));
      rdoc->SetObjectAnnotation(d, img, "basic.object.__rd_format", eNOOBDAWN_String, 0,
                                RDAnnotationHelper("float4 vertex_data;"));

      rdoc->SetObjectAnnotation(d, DefaultTriVB, "__rd_format", eNOOBDAWN_String, 0,
                                RDAnnotationHelper("float3 pos;\n"
                                                   "float4 col;\n"
                                                   "float2 uv;\n"));

      val = {};
      val.vector.float32[0] = 1.1f;
      val.vector.float32[1] = 2.2f;
      val.vector.float32[2] = 3.3f;
      val.vector.float32[3] = 4.4f;    // should be ignored
      rdoc->SetObjectAnnotation(d, img, "basic.vec3", eNOOBDAWN_Float, 3, &val);

      rdoc->SetObjectAnnotation(d, img, "deep.nested.path.to.annotation", eNOOBDAWN_Int32, 0,
                                RDAnnotationHelper(-4));
      rdoc->SetObjectAnnotation(d, img, "deep.nested.path.to.annotation2", eNOOBDAWN_Int32, 0,
                                RDAnnotationHelper(-5));
      rdoc->SetObjectAnnotation(d, img, "deep.alternate.path.to.annotation", eNOOBDAWN_Int32, 0,
                                RDAnnotationHelper(-6));

      // deleted paths should not stay around
      rdoc->SetObjectAnnotation(d, img, "deleteme", eNOOBDAWN_Int32, 0, RDAnnotationHelper(-7));
      rdoc->SetObjectAnnotation(d, img, "deleteme", eNOOBDAWN_Empty, 0, NULL);

      rdoc->SetObjectAnnotation(d, img, "path.deleted.by.parent", eNOOBDAWN_Int32, 0,
                                RDAnnotationHelper(-8));
      rdoc->SetObjectAnnotation(d, img, "path.deleted.by.parent2", eNOOBDAWN_Int32, 0,
                                RDAnnotationHelper(-9));

      // this will delete all children. `path` will still exist, but will be empty
      rdoc->SetObjectAnnotation(d, img, "path.deleted", eNOOBDAWN_Empty, 0, NULL);
    }

    while(Running())
    {
      if(rdoc)
      {
        // queue annotations don't exist in D3D11, but we set these to share the same test code.
        // we specify the immediate context here specifically. Later we'll use NULL (which is equivalent on D3D11)
        if(curFrame == 2)
          rdoc->SetCommandAnnotation(d, ctx, "queue.too_old", eNOOBDAWN_Int32, 0,
                                     RDAnnotationHelper(1000));

        rdoc->SetCommandAnnotation(d, ctx, "queue.value", eNOOBDAWN_Int32, 0,
                                   RDAnnotationHelper(1000));

        rdoc->SetCommandAnnotation(d, ctx, "command.overwritten", eNOOBDAWN_Int32, 0,
                                   RDAnnotationHelper(9999));

        rdoc->SetCommandAnnotation(d, ctx, "command.inherited", eNOOBDAWN_Int32, 0,
                                   RDAnnotationHelper(1234));

        rdoc->SetCommandAnnotation(d, ctx, "command.deleted", eNOOBDAWN_Int32, 0,
                                   RDAnnotationHelper(50));
      }

      setMarker("Start");

      if(rdoc)
      {
        rdoc->SetCommandAnnotation(d, NULL, "new.value", eNOOBDAWN_Int32, 0,
                                   RDAnnotationHelper(2000));

        rdoc->SetCommandAnnotation(d, NULL, "command.overwritten", eNOOBDAWN_Int32, 0,
                                   RDAnnotationHelper(-3333));

        rdoc->SetCommandAnnotation(d, NULL, "command.new", eNOOBDAWN_Int32, 0,
                                   RDAnnotationHelper(3333));

        rdoc->SetCommandAnnotation(d, NULL, "command.deleted", eNOOBDAWN_Empty, 0, NULL);
      }

      setMarker("Initial");

      ClearRenderTargetView(bbRTV, {0.2f, 0.2f, 0.2f, 1.0f});
      ClearRenderTargetView(imgRTV, {0.2f, 0.2f, 0.2f, 1.0f});

      if(rdoc)
        rdoc->SetCommandAnnotation(d, NULL, "command.new", eNOOBDAWN_Float, 0,
                                   RDAnnotationHelper(1.75f));

      IASetVertexBuffer(DefaultTriVB, sizeof(DefaultA2V), 0);
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ctx->IASetInputLayout(defaultLayout);

      ctx->VSSetShader(DefaultTriVS, NULL, 0);
      ctx->PSSetShader(DefaultTriPS, NULL, 0);

      RSSetViewport({0.0f, 0.0f, (float)screenWidth, (float)screenHeight, 0.0f, 1.0f});

      ctx->OMSetRenderTargets(1, &bbRTV.GetInterfacePtr(), NULL);

      setMarker("Pre-Draw");

      // deleting a value is fine if it's re-added before the next event
      if(rdoc)
      {
        rdoc->SetCommandAnnotation(d, NULL, "new.value", eNOOBDAWN_Empty, 0, NULL);
        rdoc->SetCommandAnnotation(d, NULL, "new.value", eNOOBDAWN_Int32, 0,
                                   RDAnnotationHelper(4000));
      }

      setMarker("Draw 1");

      ctx->Draw(3, 0);

      RSSetViewport({0.0f, 0.0f, (float)screenWidth / 2.0f, (float)screenHeight / 2.0f, 0.0f, 1.0f});

      setMarker("Draw 2");

      ctx->Draw(3, 0);

      Present();
    }

    return 0;
  }
};

REGISTER_TEST();
