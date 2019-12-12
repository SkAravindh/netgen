#include <mystdlib.h>
#include "meshing.hpp"

namespace netgen
{

  DLL_HEADER GeometryRegisterArray geometryregister; 
  //DLL_HEADER NgArray<GeometryRegister*> geometryregister;

  GeometryRegister :: ~GeometryRegister()
  { ; }

  Array<Point<3>> GeometryEdge :: GetEquidistantPointArray(size_t npoints) const
  {
    Array<Point<3>> pts(npoints);
    for(auto i : Range(npoints))
      pts[i] = GetPoint(double(i)/(npoints-1));
    return pts;
  }

  void GeometryFace :: RestrictHTrig(Mesh& mesh,
                                     const PointGeomInfo& gi0,
                                     const PointGeomInfo& gi1,
                                     const PointGeomInfo& gi2,
                                     const MeshingParameters& mparam,
                                     int depth, double h) const
  {
    auto p0 = GetPoint(gi0);
    auto p1 = GetPoint(gi1);
    auto p2 = GetPoint(gi2);
    auto longest = (p0-p1).Length();
    int cutedge = 2;
    if(auto len = (p0-p2).Length(); len > longest)
      {
        longest = len;
        cutedge = 1;
      }
    if(auto len = (p1-p2).Length(); len > longest)
      {
        longest = len;
        cutedge = 0;
      }
    PointGeomInfo gi_mid;
    gi_mid.u = (gi0.u + gi1.u + gi2.u)/3;
    gi_mid.v = (gi0.v + gi1.v + gi2.v)/3;

    if(depth % 3 == 0)
      {
        double curvature = 0.;
        curvature = max({curvature, GetCurvature(gi_mid),
                         GetCurvature(gi0), GetCurvature(gi1),
                         GetCurvature(gi2)});
        if(curvature < 1e-3)
          return;
        double kappa = curvature * mparam.curvaturesafety;
        h = mparam.maxh * kappa < 1 ? mparam.maxh : 1./kappa;
        if(h < 1e-4 * longest)
          return;
      }

    if(h < longest && depth < 10)
      {
        if(cutedge == 0)
          {
            PointGeomInfo gi_m;
            gi_m.u = 0.5 * (gi1.u + gi2.u);
            gi_m.v = 0.5 * (gi1.v + gi2.v);
            RestrictHTrig(mesh, gi_m, gi2, gi0, mparam, depth+1, h);
            RestrictHTrig(mesh, gi_m, gi0, gi1, mparam, depth+1, h);
          }
        else if(cutedge == 1)
          {
            PointGeomInfo gi_m;
            gi_m.u = 0.5 * (gi0.u + gi2.u);
            gi_m.v = 0.5 * (gi0.v + gi2.v);
            RestrictHTrig(mesh, gi_m, gi1, gi2, mparam, depth+1, h);
            RestrictHTrig(mesh, gi_m, gi0, gi1, mparam, depth+1, h);
          }
        else if(cutedge == 2)
          {
            PointGeomInfo gi_m;
            gi_m.u = 0.5 * (gi0.u + gi1.u);
            gi_m.v = 0.5 * (gi0.v + gi1.v);
            RestrictHTrig(mesh, gi_m, gi1, gi2, mparam, depth+1, h);
            RestrictHTrig(mesh, gi_m, gi2, gi0, mparam, depth+1, h);
          }
      }
    else
      {
        auto pmid = GetPoint(gi_mid);
        for(const auto& p : {p0, p1, p2, pmid})
          mesh.RestrictLocalH(p, h);
      }
  }

  void NetgenGeometry :: Analyse(Mesh& mesh,
                                 const MeshingParameters& mparam) const
  {
    static Timer t1("SetLocalMeshsize"); RegionTimer regt(t1);
    mesh.SetGlobalH(mparam.maxh);
    mesh.SetMinimalH(mparam.minh);

    mesh.SetLocalH(bounding_box.PMin(), bounding_box.PMax(),
                   mparam.grading);

    if(mparam.uselocalh)
      {
        double eps = 1e-12 * bounding_box.Diam();

        // restrict meshsize on edges
        for(const auto & edge : edges)
          {
            auto length = edge->GetLength();
            // skip very short edges
            if(length < eps)
              continue;
            static constexpr int npts = 20;
            // restrict mesh size based on edge length
            for(auto i : Range(npts+1))
              mesh.RestrictLocalH(edge->GetPoint(double(i)/npts), length/mparam.segmentsperedge);

            // restrict mesh size based on edge curvature
            double t = 0.;
            auto p_old = edge->GetPoint(t);
            while(t < 1.-eps)
              {
                t += edge->CalcStep(t, 1./mparam.curvaturesafety);
                if(t < 1.)
                  {
                    auto p = edge->GetPoint(t);
                    auto dist = (p-p_old).Length();
                    mesh.RestrictLocalH(p, dist);
                    p_old = p;
                  }
              }
          }

        // restrict meshsize on faces
        for(const auto& face : faces)
          face->RestrictH(mesh, mparam);
      }
    mesh.LoadLocalMeshSize(mparam.meshsizefilename);
  }

  void NetgenGeometry :: FindEdges(Mesh& mesh,
                                   const MeshingParameters& mparam) const
  {
    static Timer t1("MeshEdges"); RegionTimer regt(t1);
    static Timer tdivide("Divide Edges");
    static Timer tdivedgesections("Divide edge sections");

    // create face descriptors and set bc names
    mesh.SetNBCNames(faces.Size());
    for(auto i : Range(faces.Size()))
      {
        mesh.SetBCName(i, faces[i]->GetName());
        // todo find attached solids
        FaceDescriptor fd(i+1, 1, 0, i+1);
        fd.SetBCName(mesh.GetBCNamePtr(i));
        mesh.AddFaceDescriptor(fd);
      }

    std::map<size_t, PointIndex> vert2meshpt;
    for(auto i : Range(vertices))
      {
        const auto& vert = *vertices[i];
        MeshPoint mp(vert.GetPoint());
        vert2meshpt[vert.GetHash()] = mesh.AddPoint(mp);
      }

    size_t segnr = 0;
    for(auto facenr : Range(faces.Size()))
      {
        const auto& face = *faces[facenr];
        for(auto facebndnr : Range(face.GetNBoundaries()))
          {
            auto boundary = face.GetBoundary(facebndnr);
            for(auto enr : Range(boundary))
              {
                const auto& oriented_edge = *boundary[enr];
                auto edgenr = GetEdgeIndex(oriented_edge);
                const auto& edge = edges[edgenr];
                PointIndex startp, endp;
                // throws if points are not found
                startp = vert2meshpt.at(edge->GetStartVertex().GetHash());
                endp = vert2meshpt.at(edge->GetEndVertex().GetHash());

                // ignore collapsed edges
                if(startp == endp && edge->GetLength() < 1e-10 * bounding_box.Diam())
                  continue;
                Array<MeshPoint> mps;
                Array<double> params;
                // -------------------- DivideEdge -----------------
                static constexpr size_t divide_edge_sections = 1000;
                tdivide.Start();
                double hvalue[divide_edge_sections+1];
                hvalue[0] = 0;

                // calc local h for edge
                tdivedgesections.Start();
                auto edgepts = edge->GetEquidistantPointArray(divide_edge_sections+1);
                for(auto i : Range(edgepts.Size()-1))
                  hvalue[i+1] = hvalue[i] + 1./mesh.GetH(edgepts[i+1]) * (edgepts[i+1]-edgepts[i]).Length();
                int nsubedges = max2(1, int(floor(hvalue[divide_edge_sections]+0.5)));
                tdivedgesections.Stop();
                mps.SetSize(nsubedges-1);
                params.SetSize(nsubedges+1);

                int i = 1;
                int i1 = 0;
                do
                  {
                    if (hvalue[i1]/hvalue[divide_edge_sections]*nsubedges >= i)
                      {
                        params[i] = (double(i1)/divide_edge_sections);
                        mps[i-1] = MeshPoint(edge->GetPoint(params[i]));
                        i++;
                      }
                    i1++;
                    if (i1 > divide_edge_sections)
                      {
                        nsubedges = i;
                        mps.SetSize(nsubedges-1);
                        params.SetSize(nsubedges+1);
                        cout << "divide edge: local h too small" << endl;
                      }

                  } while(i < nsubedges);

                params[0] = 0.;
                params[nsubedges] = 1.;

                if(params[nsubedges] <= params[nsubedges-1])
                  {
                    cout << "CORRECTED" << endl;
                    mps.SetSize (nsubedges-2);
                    params.SetSize (nsubedges);
                    params[nsubedges] = 1.;
                  }
                tdivide.Stop();
                // ----------- Add Points to mesh and create segments -----
                Array<PointIndex> pnums(mps.Size() + 2);
                pnums[0] = startp;
                pnums[mps.Size()+1] = endp;

                double eps = bounding_box.Diam() * 1e-8;

                for(auto i : Range(mps))
                  {
                    bool exists = false;
                    for(auto pi : Range(mesh.Points()))
                      {
                        if((mesh[pi] - mps[i]).Length() < eps)
                          {
                            exists = true;
                            pnums[i+1] = pi;
                            break;
                          }
                      }
                    if(!exists)
                      pnums[i+1] = mesh.AddPoint(mps[i]);
                  }

                for(auto i : Range(pnums.Size()-1))
                  {
                    segnr++;
                    Segment seg;
                    seg[0] = pnums[i];
                    seg[1] = pnums[i+1];
                    seg.edgenr = segnr;
                    seg.epgeominfo[0].dist = params[i];
                    seg.epgeominfo[1].dist = params[i+1];
                    seg.epgeominfo[0].edgenr = edgenr;
                    seg.epgeominfo[1].edgenr = edgenr;
                    seg.si = facenr+1;
                    seg.surfnr1 = facenr+1;

                    // TODO: implement functionality to transfer edge parameter t to face parameters u,v
                    for(auto j : Range(2))
                      face.CalcEdgePointGI(*edge, params[i+j],
                                           seg.epgeominfo[j]);

                    if(!oriented_edge.OrientedLikeGlobal())
                      {
                        swap (seg[0], seg[1]);
                        swap (seg.epgeominfo[0].dist, seg.epgeominfo[1].dist);
                        swap (seg.epgeominfo[0].u, seg.epgeominfo[1].u);
                        swap (seg.epgeominfo[0].v, seg.epgeominfo[1].v);
                      }
                    mesh.AddSegment(seg);
                  }
              }
          }
      }
  }

  void NetgenGeometry :: MeshSurface(Mesh& mesh,
                                     const MeshingParameters& mparam) const
  {
    static Timer t1("Surface Meshing"); RegionTimer regt(t1);

    Array<int, PointIndex> glob2loc(mesh.GetNP());
    for(auto k : Range(faces))
      {
        const auto& face = *faces[k];
        auto bb = face.GetBoundingBox();
        bb.Increase(bb.Diam()/10);
        Meshing2 meshing(*this, mparam, bb);
        glob2loc = 0;
        int cntp = 0;

        for(auto& seg : mesh.LineSegments())
          {
            if(seg.si == k+1)
              {
                for(auto j : Range(2))
                  {
                    auto pi = seg[j];
                    if(glob2loc[pi] == 0)
                      {
                        meshing.AddPoint(mesh[pi], pi);
                        cntp++;
                        glob2loc[pi] = cntp;
                      }
                  }
              }
          }
        for(auto & seg : mesh.LineSegments())
          {
            if(seg.si == k+1)
              {
                PointGeomInfo gi0, gi1;
                gi0.trignum = gi1.trignum = k+1;
                gi0.u = seg.epgeominfo[0].u;
                gi0.v = seg.epgeominfo[0].v;
                gi1.u = seg.epgeominfo[1].u;
                gi1.v = seg.epgeominfo[1].v;
                meshing.AddBoundaryElement(glob2loc[seg[0]],
                                           glob2loc[seg[1]],
                                           gi0, gi1);
              }
          }

        // TODO Set max area 2* area of face

        auto noldsurfels = mesh.GetNSE();


        static Timer t("GenerateMesh"); RegionTimer reg(t);
        MESHING2_RESULT res = meshing.GenerateMesh(mesh, mparam, mparam.maxh, k+1);

        for(auto i : Range(noldsurfels, mesh.GetNSE()))
          {
            mesh.SurfaceElements()[i].SetIndex(k+1);
          }
      }
  }

  void NetgenGeometry :: OptimizeSurface(Mesh& mesh, const MeshingParameters& mparam) const
  {
    const auto savetask = multithread.task;
    multithread.task = "Optimizing surface";

    static Timer timer_opt2d("Optimization 2D");
    RegionTimer reg(timer_opt2d);
    auto meshopt = MeshOptimize2d(mesh);
    for(auto i : Range(mparam.optsteps2d))
      {
        PrintMessage(2, "Optimization step ", i);
        for(auto optstep : mparam.optimize2d)
          {
            switch(optstep)
              {
              case 's':
                meshopt.EdgeSwapping(0);
                break;
              case 'S':
                meshopt.EdgeSwapping(1);
                break;
              case 'm':
                meshopt.ImproveMesh(mparam);
                break;
              case 'c':
                meshopt.CombineImprove();
                break;
              }
          }
      }
    mesh.CalcSurfacesOfNode();
    mesh.Compress();
    multithread.task = savetask;
  }
  
  shared_ptr<NetgenGeometry> GeometryRegisterArray :: LoadFromMeshFile (istream & ist) const
  {
    for (int i = 0; i < Size(); i++)
      {
        NetgenGeometry * hgeom = (*this)[i]->LoadFromMeshFile (ist);
        if (hgeom)
          return shared_ptr<NetgenGeometry>(hgeom);
      }
    return nullptr;
  }



  
  int NetgenGeometry :: GenerateMesh (shared_ptr<Mesh> & mesh, MeshingParameters & mparam)
  {
    multithread.percent = 0;

    if(mparam.perfstepsstart <= MESHCONST_ANALYSE)
      {
        if(!mesh)
          mesh = make_shared<Mesh>();
        mesh->geomtype = GetGeomType();
        Analyse(*mesh, mparam);
      }

    if(multithread.terminate || mparam.perfstepsend <= MESHCONST_ANALYSE)
      return 0;

    if(mparam.perfstepsstart <= MESHCONST_MESHEDGES)
      FindEdges(*mesh, mparam);

    if(multithread.terminate || mparam.perfstepsend <= MESHCONST_MESHEDGES)
      return 0;

    if (mparam.perfstepsstart <= MESHCONST_MESHSURFACE)
      {
        MeshSurface(*mesh, mparam);
        mesh->CalcSurfacesOfNode();
      }

    if (multithread.terminate || mparam.perfstepsend <= MESHCONST_MESHSURFACE)
      return 0;

    if (mparam.perfstepsstart <= MESHCONST_OPTSURFACE)
      OptimizeSurface(*mesh, mparam);

    if (multithread.terminate || mparam.perfstepsend <= MESHCONST_OPTSURFACE)
      return 0;


    if(mparam.perfstepsstart <= MESHCONST_MESHVOLUME)
      {
        multithread.task = "Volume meshing";

        MESHING3_RESULT res = MeshVolume (mparam, *mesh);

        if (res != MESHING3_OK) return 1;
        if (multithread.terminate) return 0;

        RemoveIllegalElements (*mesh);
        if (multithread.terminate) return 0;

        MeshQuality3d (*mesh);
      }

    if (multithread.terminate || mparam.perfstepsend <= MESHCONST_MESHVOLUME)
      return 0;


    if (mparam.perfstepsstart <= MESHCONST_OPTVOLUME)
      {
	multithread.task = "Volume optimization";

	OptimizeVolume (mparam, *mesh);
	if (multithread.terminate) return 0;
      }
    FinalizeMesh(*mesh);
    return 0;
  }

  void NetgenGeometry :: Save (string filename) const
  {
    throw NgException("Cannot save geometry - no geometry available");
  }

  static RegisterClassForArchive<NetgenGeometry> regnggeo;
}
