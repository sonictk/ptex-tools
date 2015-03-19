#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "objreader.hpp"

#include "ptex_util.hpp"

using std::vector;

template <typename T>
struct releaser {
    void operator()(T *r) const {
        r->release();
    }
};

using MetaPtr = std::unique_ptr<PtexMetaData, releaser<PtexMetaData> >;

struct InputTex {
    InputTex(int ioffset, int mesh_offset, PtexTexture *tex)
	:offset(ioffset), mesh_offset(mesh_offset), ptex(tex){}
    int offset;
    int mesh_offset;
    PtexTexture *ptex;
};

typedef vector<InputTex> InputList;
struct InputInfo {
    ~InputInfo() {
	for (InputList::iterator it = inputs.begin();
	     it != inputs.end(); ++it){
	    it->ptex->release();
	}
    }
    Ptex::DataType data_type;
    Ptex::MeshType mesh_type;
    int alpha_channel;
    int num_channels;
    int num_faces;
    bool merge_mesh;
    obj_mesh mesh;
    InputList inputs;
};

static
int append_mesh(obj_mesh &mesh, PtexTexture *tex) {
    MetaPtr meta(tex->getMetaData());

    const int32_t *nverts;
    const int32_t *verts;
    const float *pos;
    int face_count, verts_count, pos_count;
    meta->getValue("PtexFaceVertCounts", nverts, face_count);
    if (nverts == 0)
        return 0;
    meta->getValue("PtexFaceVertIndices", verts, verts_count);
    if (verts == 0)
        return 0;
    meta->getValue("PtexVertPositions", pos, pos_count);
    if (pos == 0)
        return 0;

    mesh.nverts.insert(end(mesh.nverts), nverts, nverts+face_count);
    int offset = mesh.pos.size()/3;

    mesh.verts.reserve(mesh.verts.size() + verts_count);

    std::transform(verts, verts+verts_count, std::back_inserter(mesh.verts),
                   [&](int32_t v)->int32_t { return v+offset; });
    mesh.pos.insert(end(mesh.pos), pos, pos+pos_count);

    return face_count;
}

static
int append_input(InputInfo *info, const char* filename, Ptex::String &err_msg) {
    PtexTexture* ptex(PtexTexture::open(filename, err_msg, 0));
    if (!ptex) {
	return -1;
    }
    if (ptex->dataType() != info->data_type){
	std::string err = std::string("Data type does not match with first file: ")
	    + filename
	    + " expected: " + Ptex::DataTypeName(info->data_type)
	    + " got:" + Ptex::DataTypeName(ptex->dataType());
	err_msg = err.c_str();
	return -1;
    }
    if (ptex->meshType() != info->mesh_type){
	std::string err = std::string("Mesh type does not match with first file: ")
	    +filename;
	err_msg = err.c_str();
	return -1;
    }
    if (ptex->numChannels() != info->num_channels){
	std::string err = std::string("Number of channels does not match wifh first file: ")
	    + filename;
	err_msg = err.c_str();
	return -1;
    }

    if (ptex->alphaChannel() != info->alpha_channel){
	std::string err = std::string("Alpha channel does not match wifh first file: ")
	    +filename;
	err_msg = err.c_str();
	return -1;
    }
    int mesh_offset = 0;
    if (info->merge_mesh) {
        mesh_offset = info->mesh.nverts.size();
        int nfaces = append_mesh(info->mesh, ptex);
        info->merge_mesh = nfaces > 0;
    }

    info->inputs.push_back(InputTex(info->num_faces, mesh_offset, ptex));
    info->num_faces += ptex->numFaces();

    return 0;
}

static
int append_ptexture(PtexWriter *writer, int offset, PtexTexture *ptex){
    int num_faces = ptex->numFaces();
    int data_block_size = Ptex::DataSize(ptex->dataType()) * ptex->numChannels();
    int data_size = data_block_size*1024;
    void *data = malloc(data_size);
    for (int i=0; i < num_faces; ++i) {
	Ptex::FaceInfo face_info = ptex->getFaceInfo(i);
	//copy dat shit
	Ptex::FaceInfo outf = face_info;
	outf.adjfaces[0] = outf.adjfaces[0] == -1 ? -1 : outf.adjfaces[0] + offset;
	outf.adjfaces[1] = outf.adjfaces[1] == -1 ? -1 : outf.adjfaces[1] + offset;
	outf.adjfaces[2] = outf.adjfaces[2] == -1 ? -1 : outf.adjfaces[2] + offset;
	outf.adjfaces[3] = outf.adjfaces[3] == -1 ? -1 : outf.adjfaces[3] + offset;

	int req_size = outf.res.size()*data_block_size;
	if (req_size > data_size){
	    data_size = req_size;
	    data = realloc(data, data_size);
	}
	ptex->getData(i, data,0);
	if (face_info.isConstant())
	    writer->writeConstantFace(offset+i, outf, data);
	else
	    writer->writeFace(offset+i, outf, data);
    }
    free(data);
    return 0;
}

int ptex_tools::ptex_merge(int nfiles, const char** files,
                           const char*output_file, int *offsets,
                           Ptex::String &err_msg){

    InputInfo info;
    if (nfiles < 2){
	err_msg = "At least 2 files needed";
	return -1;
    }
    if (output_file == 0){
	err_msg = "Output file is null";
	return -1;
    }
    PtexTexture* first(PtexTexture::open(files[0], err_msg, 0));
    if (!first) {
	return -1;
    }
    info.data_type = first->dataType();
    info.mesh_type = first->meshType();
    info.num_channels = first->numChannels();
    info.alpha_channel = first->alphaChannel();
    info.num_faces = first->numFaces();
    int nfaces = append_mesh(info.mesh, first);
    info.merge_mesh = nfaces > 0;
    info.inputs.push_back(InputTex(0, 0, first));

    for (int i = 1; i < nfiles; i++){
	if (append_input(&info, files[i], err_msg))
	    return -1;
    }
    PtexWriter *writer = PtexWriter::open(output_file,
					  info.mesh_type,
					  info.data_type,
					  info.num_channels,
					  info.alpha_channel,
					  info.num_faces,
					  err_msg);
    if (!writer)
	return -1;
    for (InputList::iterator it = info.inputs.begin();
	     it != info.inputs.end(); ++it)
    {
	if(append_ptexture(writer, it->offset, it->ptex))
	    return -1;
	int idx = std::distance(info.inputs.begin(), it);
	offsets[idx] = it->offset;
    }

    if (info.merge_mesh) {
        writer->writeMeta("PtexFaceVertCounts",
                          info.mesh.nverts.data(),
                          info.mesh.nverts.size());
        writer->writeMeta("PtexFaceVertIndices",
                          info.mesh.verts.data(),
                          info.mesh.verts.size());
        writer->writeMeta("PtexVertPositions",
                          info.mesh.pos.data(),
                          info.mesh.pos.size());
    }

    if (!writer->close(err_msg)){
	writer->release();
	return -1;
    }
    writer->release();
    return 0;
}
