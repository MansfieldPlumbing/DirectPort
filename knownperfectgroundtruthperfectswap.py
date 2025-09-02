"""
VirtuaCast Ground Truth Generator (Definitive Version)

This script is a self-contained, faithful port of the working pipeline
from the provided Tegrity Engine reference files. It uses the correct models,
data normalization, and the critical EMAP transformation.

Instructions:
  1. Modify file paths in the CONFIG dictionary below if needed.
  2. Run from the project root: > python scripts/groundtruth.py
"""

import os
import cv2
import numpy as np
import onnxruntime as ort
import onnx
from onnx import numpy_helper

# --- CONFIGURATION ---
CONFIG = {
    "source_image_path": "../Sources/Alyson Hannigan.jpg",
    "target_image_path": "test_data/target_frame.jpg",
    "output_image_path": "python_final_swap.jpg",
    "model_directory": "../models"
}
# ---------------------

# --- ANSI Color Codes for Output ---
C_BLUE, C_GREEN, C_RED, C_RESET = '\033[94m', '\033[92m', '\033[91m', '\033[0m'

# --- Core Algorithm Replication (Ported from tegrity_core.py) ---

def estimate_norm(lmk, image_size=112):
    """Replicates the scikit-image SimilarityTransform estimation using OpenCV."""
    arcface_dst = np.array([
        [38.2946, 51.6963], [73.5318, 51.5014], [56.0252, 71.7366],
        [41.5493, 92.3655], [70.7299, 92.2041]], dtype=np.float32)
    ratio = float(image_size) / 112.0
    dst = arcface_dst * ratio
    tform = cv2.estimateAffinePartial2D(lmk.astype(np.float32), dst)[0]
    return tform

def distance2bbox(points, distance):
    x1 = points[:, 0] - distance[:, 0]
    y1 = points[:, 1] - distance[:, 1]
    x2 = points[:, 0] + distance[:, 2]
    y2 = points[:, 1] + distance[:, 3]
    return np.stack([x1, y1, x2, y2], axis=-1)

def distance2kps(points, distance):
    preds = []
    for i in range(0, distance.shape[1], 2):
        px = points[:, 0] + distance[:, i]
        py = points[:, 1] + distance[:, i + 1]
        preds.append(px)
        preds.append(py)
    return np.stack(preds, axis=-1)

class FaceDetector:
    """A direct port of the RetinaFace class from your tegrity_core.py reference."""
    def __init__(self, model_path):
        self.session = ort.InferenceSession(model_path, providers=['DmlExecutionProvider', 'CPUExecutionProvider'])
        self.center_cache = {}
        self.nms_thresh = 0.4
        self.det_thresh = 0.5
        
        input_cfg = self.session.get_inputs()[0]
        self.input_name = input_cfg.name
        input_shape = list(input_cfg.shape)
        
        if not isinstance(input_shape[2], int) or not isinstance(input_shape[3], int):
            input_shape[2], input_shape[3] = 640, 640
        self.input_size = tuple(input_shape[2:4][::-1])

        self.output_names = [o.name for o in self.session.get_outputs()]
        self._feat_stride_fpn = [8, 16, 32]
        self._num_anchors = 2
        self.fmc = 3 # feature map count per stride

    def forward(self, img_blob):
        net_outs = self.session.run(self.output_names, {self.input_name: img_blob})
        input_height, input_width = img_blob.shape[2], img_blob.shape[3]
        scores_list, bboxes_list, kpss_list = [], [], []

        for idx, stride in enumerate(self._feat_stride_fpn):
            scores = net_outs[idx]
            bbox_preds = net_outs[idx + self.fmc] * stride
            kps_preds = net_outs[idx + self.fmc * 2] * stride
            
            height, width = input_height // stride, input_width // stride
            
            anchor_centers = self.center_cache.get((height, width, stride))
            if anchor_centers is None:
                anchor_centers = np.stack(np.mgrid[:height, :width][::-1], axis=-1).astype(np.float32)
                anchor_centers = (anchor_centers * stride).reshape((-1, 2))
                anchor_centers = np.stack([anchor_centers] * self._num_anchors, axis=1).reshape((-1, 2))
                self.center_cache[(height, width, stride)] = anchor_centers

            scores = scores.transpose((0, 2, 3, 1)).reshape(-1)
            pos_inds = np.where(scores >= self.det_thresh)[0]
            
            bboxes = distance2bbox(anchor_centers[pos_inds], bbox_preds.transpose((0, 2, 3, 1)).reshape(-1, 4)[pos_inds])
            kpss = distance2kps(anchor_centers[pos_inds], kps_preds.transpose((0, 2, 3, 1)).reshape(-1, 10)[pos_inds])
            
            scores_list.append(scores[pos_inds])
            bboxes_list.append(bboxes)
            kpss_list.append(kpss)
        return scores_list, bboxes_list, kpss_list

    def detect(self, img):
        im_ratio = float(img.shape[0]) / img.shape[1]
        model_ratio = float(self.input_size[1]) / self.input_size[0]
        new_height = self.input_size[1] if im_ratio > model_ratio else int(self.input_size[0] * im_ratio)
        new_width = int(self.input_size[1] / im_ratio) if im_ratio > model_ratio else self.input_size[0]
        det_scale = float(new_height) / img.shape[0]

        resized_img = cv2.resize(img, (new_width, new_height))
        det_img = np.zeros((self.input_size[1], self.input_size[0], 3), dtype=np.uint8)
        det_img[:new_height, :new_width, :] = resized_img
        
        blob = cv2.dnn.blobFromImage(det_img, 1.0/128.0, self.input_size, (127.5, 127.5, 127.5), swapRB=True)
        scores_list, bboxes_list, kpss_list = self.forward(blob)

        if not scores_list: return []
        
        scores = np.vstack(scores_list).ravel()
        bboxes = np.vstack(bboxes_list) / det_scale
        kpss = np.vstack(kpss_list) / det_scale

        order = scores.argsort()[::-1]
        pre_det = np.hstack((bboxes, scores[:, np.newaxis]))[order, :]
        
        keep = cv2.dnn.NMSBoxes(pre_det[:,:4].tolist(), pre_det[:,4].tolist(), self.det_thresh, self.nms_thresh)
        if keep is None or len(keep) == 0: return []
        
        indices = keep.flatten()
        final_kpss = kpss[order][indices]
        
        return [{'landmarks': final_kpss[i].reshape(5, 2)} for i in range(len(indices))]


def extract_emap_from_model(model_path):
    try:
        model = onnx.load(model_path)
        for initializer in model.graph.initializer:
            if initializer.name == 'buff2fs':
                arr = numpy_helper.to_array(initializer)
                if arr.shape == (512, 512):
                    print(f"Found and extracted EMAP initializer: '{initializer.name}'")
                    return arr.astype(np.float32)
    except Exception as e:
        print(f"{C_RED}Could not read ONNX model for EMAP extraction: {e}{C_RESET}")
    return None

def run_pipeline(config):
    print(f"\n{C_BLUE}--- VirtuaCast Ground Truth Generator ---{C_RESET}")

    det_model_path = os.path.join(config['model_directory'], "det_10g.onnx")
    rec_model_path = os.path.join(config['model_directory'], "w600k_r50.onnx")
    swap_model_path = os.path.join(config['model_directory'], "inswapper_128.onnx")

    try:
        print("Loading ONNX models...")
        face_detector = FaceDetector(model_path=det_model_path)
        rec_session = ort.InferenceSession(rec_model_path, providers=['DmlExecutionProvider', 'CPUExecutionProvider'])
        swap_session = ort.InferenceSession(swap_model_path, providers=['DmlExecutionProvider', 'CPUExecutionProvider'])
        print(f"{C_GREEN}Models loaded successfully using: {ort.get_device()}{C_RESET}")
    except Exception as e:
        print(f"{C_RED}ERROR: Failed to load ONNX models. {e}{C_RESET}"); return

    emap = extract_emap_from_model(swap_model_path)
    if emap is None: print(f"{C_RED}ERROR: Could not find EMAP matrix in '{swap_model_path}'.{C_RESET}"); return

    print("Loading images...")
    source_img = cv2.imread(config['source_image_path'])
    target_img = cv2.imread(config['target_image_path'])
    if source_img is None: print(f"{C_RED}ERROR: Failed to load source: '{config['source_image_path']}'{C_RESET}"); return
    if target_img is None: print(f"{C_RED}ERROR: Failed to load target: '{config['target_image_path']}'{C_RESET}"); return

    print("Processing source face...")
    source_faces = face_detector.detect(source_img)
    if not source_faces: print(f"{C_RED}ERROR: No face found in source image.{C_RESET}"); return
    
    source_face = source_faces[0]
    M_align_src = estimate_norm(source_face['landmarks'], image_size=112)
    aligned_source_face = cv2.warpAffine(source_img, M_align_src, (112, 112), borderValue=0.0)
    rec_blob = cv2.dnn.blobFromImage(aligned_source_face, 1.0 / 127.5, (112, 112), (127.5, 127.5, 127.5), swapRB=True)
    source_embedding = rec_session.run(None, {'input.1': rec_blob})[0]
    source_embedding /= np.linalg.norm(source_embedding)

    print("Processing target face and swapping...")
    target_faces = face_detector.detect(target_img)
    if not target_faces: print(f"{C_RED}ERROR: No face found in target image.{C_RESET}"); return
    
    target_face = target_faces[0]
    M_align_tgt = estimate_norm(target_face['landmarks'], image_size=128)
    aligned_target_face = cv2.warpAffine(target_img, M_align_tgt, (128, 128), borderValue=0.0)
    
    swap_blob = cv2.dnn.blobFromImage(aligned_target_face, 1.0/255.0, (128, 128), (0.0, 0.0, 0.0), swapRB=True)
    latent = np.dot(source_embedding, emap)
    latent /= np.linalg.norm(latent)
    
    swapped_face_raw = swap_session.run(None, {'target': swap_blob, 'source': latent})[0][0]
    
    swapped_face_img = np.clip(255 * swapped_face_raw.transpose((1, 2, 0)), 0, 255).astype(np.uint8)
    swapped_face_img = cv2.cvtColor(swapped_face_img, cv2.COLOR_RGB2BGR)

    print("Blending result...")
    try:
        M_paste = cv2.invertAffineTransform(M_align_tgt)
    except cv2.error:
        print(f"{C_RED}ERROR: Alignment matrix for target face was non-invertible.{C_RESET}"); return

    full_frame_swapped_face = cv2.warpAffine(swapped_face_img, M_paste, (target_img.shape[1], target_img.shape[0]))
    mask = np.full((128, 128), 255, dtype=np.uint8)
    full_frame_mask = cv2.warpAffine(mask, M_paste, (target_img.shape[1], target_img.shape[0]))
    full_frame_mask_blurred = cv2.GaussianBlur(full_frame_mask, (21, 21), 0)
    mask_float = (full_frame_mask_blurred.astype(np.float32) / 255.0)[:, :, np.newaxis]
    final_result = (full_frame_swapped_face * mask_float + target_img * (1 - mask_float)).astype(np.uint8)
    
    print(f"Saving final image to '{config['output_image_path']}'...")
    cv2.imwrite(config['output_image_path'], final_result)
    print(f"{C_GREEN}--- Ground Truth Generation Complete ---{C_RESET}")

if __name__ == '__main__':
    run_pipeline(CONFIG)