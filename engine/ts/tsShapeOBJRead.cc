//-----------------------------------------------------------------------------
// Torque Game Engine
// Copyright (C) GarageGames.com, Inc.
//-----------------------------------------------------------------------------

#include "core/memstream.h"
#include "util/safeDelete.h"
#include "ts/tsShape.h"

// for DTS SDK...
#include <string>
#include <strstream> // deprecated, with no alternatives!

// Steal DTS SDK for our purposes
#include "../lib/dtsSDK/DTSShape.h"

//------------------------------------------------------
// DTS interface
//------------------------------------------------------

namespace DTS {
    //! Point imported from OBJ coordinates
    struct OBJPoint : public Point
    {
        OBJPoint(const Point p) : Point(-p[0], p[2], p[1]) {}
    };

    class OBJMesh : public Mesh
    {
    public:
        void assemble(class OBJShape* shape, S32 rootNode);

        OBJMesh(class OBJShape* shape, S32 rootNode): Mesh (Mesh::T_Standard)
        {
            assemble(shape, rootNode);
        }
    };

    class OBJShape : public Shape
    {
        friend OBJMesh;
        std::vector<Point> mObjPoints;
        std::vector<Point> mObjNormals;
        std::vector<Point2D> mObjTexcoord;

        struct Face { S32 v[3], n[3], t[3]; };
        std::vector<Face> mObjFaces;

    public:
        void parse(Stream& s)
        {
            const int lineMaxSize = 1024;
            char line[lineMaxSize];
            while (s.getStatus() != Stream::EOS) {
                s.readLine((U8*) line, lineMaxSize);
                char* start = line;

                S32 lspace = 0;
                if (lspace = dStrspn(start, " \t"))
                    lspace += dStrcspn(start, " \t");
                if (lspace < lineMaxSize && (start[lspace] == '#' || start[lspace] == '\0'))
                    continue; // This is a comment or a blank line
                
                start += lspace;
                
                char type[3]; // line type (f, v, vt, vn ...)
                S32 valuesStart = 0;
                S32 hasLineType = dSscanf(start, "%2s %n", type, &valuesStart);
                if (hasLineType) {
                    start += valuesStart;

                    if (dStrcmp(type, "v") == 0) // vertex
                    {
                        F32 x, y, z;
                        dSscanf(start, "%f %f %f", &x, &y, &z);
                        mObjPoints.emplace_back(x, y, z);
                    }
                    else if (dStrcmp(type, "vt") == 0) // texcoord
                    {
                        F32 s, t;
                        dSscanf(start, "%f %f", &s, &t);
                        mObjTexcoord.emplace_back(s, t);
                    }
                    else if (dStrcmp(type, "vn") == 0) // normal
                    {
                        F32 nx, ny, nz;
                        dSscanf(start, "%f %f %f", &nx, &ny, &nz);
                        mObjNormals.emplace_back(nx, ny, nz);
                    }
                    else if (dStrcmp(type, "f") == 0) // face
                    {
                        char sets[3][64];
                        if (dSscanf(start, "%63s %63s %63s", sets[0], sets[1], sets[2]) >= 3) {
                            Face face;
                            for (S32 i = 0; i < 3; ++i) {
                                char* set = sets[i];

                                S32 vert;
                                S32 norm = 0;
                                S32 texc = 0;
                                char* firstDiv = dStrchr(set, '/');
                                if (firstDiv)
                                    *firstDiv = 0;
                                vert = dAtoi(set);
                                if (!firstDiv) // it's a single position index (v)
                                    goto endvertex;
                                char* secondDiv = dStrchr(firstDiv+1, '/');
                                if (secondDiv)
                                    *secondDiv = 0;
                                else { // vertex and texcoord only "v/t"
                                    texc = dAtoi(firstDiv+1);
                                    goto endvertex;
                                }
                                if (firstDiv+1 != secondDiv) { // texcoord data not omitted "v/t/n"
                                    texc = dAtoi(firstDiv+1);
                                    norm = dAtoi(secondDiv+1);
                                }
                            endvertex:
                                // OBJ is 1-indexed, our lists aren't, so correct that too.
                                face.v[i] = vert-1;
                                face.n[i] = norm-1;
                                face.t[i] = texc-1;
                            }
                            mObjFaces.push_back(dMove(face));
                        }
                    }
                }
            }
        }

        OBJShape(Stream& s)
        {
            // this fills the mObj* vectors.
            parse(s);

            // OBJ models have no node hierarchy, but shapes need at least one root node.
            Node n;
            n.name = addName("Root");
            n.parent = -1;
            nodes.push_back(n);

            // Node indexes map one to one with the default rotation and
            // tranlation arrays: push identity transform for the Root.
            nodeDefRotations.push_back(Quaternion(0,0,0,1));
            nodeDefTranslations.push_back(Point(0,0,0));

            // One single mesh object
            Object o;
            o.name      = addName("Mesh");
            o.numMeshes = 1;
            o.firstMesh = 0;
            o.node = nodes.size() - 1;

            meshes.push_back(OBJMesh(this, o.node));
            objects.push_back(o);

            // Again, one single detail level can be defined per OBJ model
            DetailLevel dl ;
            dl.name         = addName("Detail-1");
            dl.size         = 0; // Projected size of detail level
            dl.objectDetail = 0;
            dl.polyCount    = 0;
            dl.subshape     = 0;
            dl.maxError     = -1;
            dl.avgError     = -1;
            dl.polyCount = meshes.back().getPolyCount();
            detailLevels.push_back(dl);
            setSmallestSize(0);

            // Update our bounds values as we might use them to build
            // a collision mesh.
            calculateBounds();
            calculateCenter();
            calculateRadius();
            calculateTubeRadius();

            // Create a subshape with everything
            Subshape sub ;
            sub.firstObject = 0;
            sub.firstNode   = 0;
            sub.firstDecal  = 0;
            sub.numNodes    = nodes.size();
            sub.numObjects  = objects.size();
            sub.numDecals   = decals.size();
            subshapes.push_back(sub);

            // Create an object state for each object (not sure about this)
            ObjectState os;
            os.vis      = 1.0f;
            os.frame    = 0;
            os.matFrame = 0;
            for (S32 i = 0; i < objects.size(); i++)
                objectStates.push_back(os);

            // Recalculate bounds (they may have changed)
            calculateBounds();
            calculateCenter();
            calculateRadius();
            calculateTubeRadius();

            // Finally, create the material for the mesh
            Material mat;
            mat.name        = "white";
            mat.flags       = Material::NeverEnvMap;
            mat.reflectance = -1;
            mat.bump        = -1;
            mat.detail      = -1;
            mat.reflection  = 1.0f;
            mat.detailScale = 1.0f;
            materials.push_back(mat);
        }
    };

    void OBJMesh::assemble(OBJShape* shape, S32 rootNode)
    {
        S32 f, v;
        S32 materialIndex = 0; // use only one material

        // Import vertex position and texture data
        // (flatten entire model to separate vertices)
        for (f = 0; f < shape->mObjFaces.size(); ++f) {
            OBJShape::Face& tri = shape->mObjFaces[f];
            Primitive prim;

            for (v = 0; v < 3; ++v) {
                // add vertex configuration specific to this face
                Point point = shape->mObjPoints[tri.v[v]];
                verts.push_back (OBJPoint(point));

                Point2D tc = tri.t[v] < shape->mObjTexcoord.size()? shape->mObjTexcoord[tri.t[v]] : Point2D(0.0, 0.0);
                tverts.push_back (tc);

                // and compute per-vertex normals
                Point n = tri.n[v] < shape->mObjNormals.size()? shape->mObjNormals[tri.n[v]] : Point(0.0, 0.0, 0.0);
                normals.push_back(OBJPoint(n));
                enormals.push_back(encodeNormal(normals.back()));

                // Vertex weights, OBJ fully assigns this mesh to the root bone.
                // (All vertices in a skin mesh must be assigned to a bone.)
                vindex.push_back(verts.size());
                vbone.push_back(rootNode);
                vweight.push_back(1.0);
            }

            // Create a triangle primitive for this triangle
            prim.firstElement = indices.size();
            prim.numElements  = 3;
            prim.type         = Primitive::Indexed;

            if (materialIndex >= 0)
                prim.type |= materialIndex;

            indices.push_back  (prim.firstElement);
            indices.push_back  (prim.firstElement + 2);
            indices.push_back  (prim.firstElement + 1);

            primitives.push_back (prim);
        }

        // Other stuff we don't support
        setFrames(1);
        setParent(-1);
        calculateBounds();
        calculateCenter();
        calculateRadius();
    }
}

//------------------------------------------------------
// Resource manager hook
//------------------------------------------------------

ResourceInstance* constructOBJShape(Stream& s)
{
    TSShape* ret = NULL;

    // assemble DTS stream from OBJ model
    std::ostrstream out;
    DTS::OBJShape objshape(s);
    objshape.save(out);
    auto* outbuf = out.rdbuf();

    MemStream dts(outbuf->pcount(), outbuf->str(), true, false);

    ret = new TSShape;
    if (!ret->read(&dts))
    {
        // Failed, so clean up and set to NULL so we'll return NULL.
        SAFE_DELETE(ret);
    }

fail:
    return ret;
}
