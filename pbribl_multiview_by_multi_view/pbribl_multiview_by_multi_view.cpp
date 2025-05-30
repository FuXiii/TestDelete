/*
* Vulkan Example - Physical based rendering with image based lighting
*
* This sample adds imaged based lighting from an environment map to the PBR equation
*
* Copyright (C) 2016-2023 by Sascha Willems - www.saschawillems.de
*
* This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
*/

// For reference see http://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf

#include "vulkanexamplebase.h"
#include "VulkanglTFModel.h"

std::size_t row = 6;
std::size_t column = 6;

struct Material {
	// Parameter block used as push constant block
	struct PushBlock {
		float roughness = 0.0f;
		float metallic = 0.0f;
		float specular = 0.0f;
		float r, g, b;
	} params;
	std::string name;
	Material() {};
	Material(std::string n, glm::vec3 c) : name(n) {
		params.r = c.r;
		params.g = c.g;
		params.b = c.b;
	};
};

class VulkanExample : public VulkanExampleBase
{
public:
	bool displaySkybox = true;

	struct Textures {
		vks::TextureCubeMap environmentCube;
		// Generated at runtime
		vks::Texture2D lutBrdf;
		vks::TextureCubeMap irradianceCube;
		vks::TextureCubeMap prefilteredCube;
	} textures;

	struct Meshes {
		vkglTF::Model skybox;
		std::vector<vkglTF::Model> objects;
		int32_t objectIndex = 0;
	} models;

	struct {
		vks::Buffer object;
		vks::Buffer skybox;
		vks::Buffer params;
		vks::Buffer views;
	} uniformBuffers;

	struct UBOMatrices {
		glm::mat4 projection;
		glm::mat4 model;
		glm::mat4 view;
		glm::vec3 camPos;
	} uboMatrices;

	struct UBOParams {
		glm::vec4 lights[4];
		float exposure = 4.5f;
		float gamma = 2.2f;
	} uboParams;

	struct {
		VkPipeline skybox{ VK_NULL_HANDLE };
		VkPipeline pbr{ VK_NULL_HANDLE };
	} pipelines;

	struct {
		VkDescriptorSet object{ VK_NULL_HANDLE };
		VkDescriptorSet skybox{ VK_NULL_HANDLE };
	} descriptorSets;

	struct PushConstStruct
	{
		glm::vec3 pos;
		int offset;
	};

	std::array<glm::mat4, 500> views;

	VkPipelineLayout pipelineLayout{ VK_NULL_HANDLE };
	VkDescriptorSetLayout descriptorSetLayout{ VK_NULL_HANDLE };

	// Default materials to select from
	std::vector<Material> materials;
	int32_t materialIndex = 0;

	std::vector<std::string> materialNames;
	std::vector<std::string> objectNames;


	// multiview
	VkPhysicalDeviceMultiviewFeaturesKHR physicalDeviceMultiviewFeatures{};
	VkPhysicalDeviceMultiviewPropertiesKHR physicalDeviceMultiviewProperties{};

	struct MultiviewPass {
		//VkPhysicalDeviceMultiviewPropertiesKHR::maxMultiviewViewCount
		//一次最多绘制multiview有显示，为了解除该限制，只能创建多个framebuffer
		//多次渲染，每次绑定一个framebuffer，一个framebuffer渲染多个multiview
		struct FrameBufferAttachment {
			VkImage image{ VK_NULL_HANDLE };
			VkDeviceMemory memory{ VK_NULL_HANDLE };
			VkImageView view{ VK_NULL_HANDLE };

			uint32_t arrayLayers = 0;
			uint32_t usedArrayLayers = 0;
		};

		std::vector<FrameBufferAttachment> colors;
		std::vector<FrameBufferAttachment> depths;

		std::vector<VkFramebuffer> frameBuffers;

		VkRenderPass renderPass = VK_NULL_HANDLE;
	}multiviewPass;

	struct ImGuiPass
	{
		VkRenderPass renderPass = VK_NULL_HANDLE;
	}imGuiPass;

	VulkanExample() : VulkanExampleBase()
	{
		title = "PBR with image based lighting";

		camera.type = Camera::CameraType::firstperson;
		camera.movementSpeed = 4.0f;
		camera.setPerspective(60.0f, (float)width / (float)height, 0.1f, 256.0f);
		camera.rotationSpeed = 0.25f;

		camera.setRotation({ -3.75f, 180.0f, 0.0f });
		camera.setPosition({ 0.55f, 0.85f, 12.0f });

		// Setup some default materials (source: https://seblagarde.wordpress.com/2011/08/17/feeding-a-physical-based-lighting-mode/)
		materials.push_back(Material("Gold", glm::vec3(1.0f, 0.765557f, 0.336057f)));
		materials.push_back(Material("Copper", glm::vec3(0.955008f, 0.637427f, 0.538163f)));
		materials.push_back(Material("Chromium", glm::vec3(0.549585f, 0.556114f, 0.554256f)));
		materials.push_back(Material("Nickel", glm::vec3(0.659777f, 0.608679f, 0.525649f)));
		materials.push_back(Material("Titanium", glm::vec3(0.541931f, 0.496791f, 0.449419f)));
		materials.push_back(Material("Cobalt", glm::vec3(0.662124f, 0.654864f, 0.633732f)));
		materials.push_back(Material("Platinum", glm::vec3(0.672411f, 0.637331f, 0.585456f)));
		// Testing materials
		materials.push_back(Material("White", glm::vec3(1.0f)));
		materials.push_back(Material("Dark", glm::vec3(0.1f)));
		materials.push_back(Material("Black", glm::vec3(0.0f)));
		materials.push_back(Material("Red", glm::vec3(1.0f, 0.0f, 0.0f)));
		materials.push_back(Material("Blue", glm::vec3(0.0f, 0.0f, 1.0f)));
		materials.push_back(Material("Deer", glm::vec3(1.0f, 0.0f, 1.0f)));

		for (auto material : materials) {
			materialNames.push_back(material.name);
		}
		objectNames = { "Sphere", "Teapot", "Torusknot", "Venus" ,"Deer" };

		materialIndex = 9;

		{
			// Enable extension required for multiview
			enabledDeviceExtensions.push_back(VK_KHR_MULTIVIEW_EXTENSION_NAME);

			// Reading device properties and features for multiview requires VK_KHR_get_physical_device_properties2 to be enabled
			enabledInstanceExtensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);

			// Enable required extension features
			this->physicalDeviceMultiviewFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES_KHR;
			this->physicalDeviceMultiviewFeatures.multiview = VK_TRUE;
			deviceCreatepNextChain = &(this->physicalDeviceMultiviewFeatures);
		}
	}

	~VulkanExample()
	{
		if (device) {
			vkDestroyPipeline(device, pipelines.skybox, nullptr);
			vkDestroyPipeline(device, pipelines.pbr, nullptr);
			vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
			vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
			uniformBuffers.object.destroy();
			uniformBuffers.skybox.destroy();
			uniformBuffers.params.destroy();
			uniformBuffers.views.destroy();
			textures.environmentCube.destroy();
			textures.irradianceCube.destroy();
			textures.prefilteredCube.destroy();
			textures.lutBrdf.destroy();
		}
	}

	virtual void getEnabledFeatures()
	{
		if (deviceFeatures.samplerAnisotropy) {
			enabledFeatures.samplerAnisotropy = VK_TRUE;
		}
	}

	void buildCommandBuffers()
	{
		VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();

		VkClearValue clearValues[2];
		clearValues[0].color = { { 0.1f, 0.1f, 0.1f, 1.0f } };
		clearValues[1].depthStencil = { 1.0f, 0 };

		VkRenderPassBeginInfo renderPassBeginInfo = vks::initializers::renderPassBeginInfo();
		//renderPassBeginInfo.renderPass = renderPass;
		renderPassBeginInfo.renderPass = multiviewPass.renderPass;
		renderPassBeginInfo.renderArea.offset.x = 0;
		renderPassBeginInfo.renderArea.offset.y = 0;
		renderPassBeginInfo.renderArea.extent.width = width;
		renderPassBeginInfo.renderArea.extent.height = height;
		renderPassBeginInfo.clearValueCount = 2;
		renderPassBeginInfo.pClearValues = clearValues;

		for (size_t i = 0; i < drawCmdBuffers.size(); ++i)
		{
			VK_CHECK_RESULT(vkBeginCommandBuffer(drawCmdBuffers[i], &cmdBufInfo));

			for (size_t frame_buffer_index = 0; frame_buffer_index < multiviewPass.frameBuffers.size(); frame_buffer_index++)
			{
				// Set target frame buffer
				renderPassBeginInfo.framebuffer = multiviewPass.frameBuffers[frame_buffer_index];
				vkCmdBeginRenderPass(drawCmdBuffers[i], &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

				VkRect2D scissor = vks::initializers::rect2D(width, height, 0, 0);
				vkCmdSetScissor(drawCmdBuffers[i], 0, 1, &scissor);

				{
					VkViewport viewport = vks::initializers::viewport((float)width, (float)height, 0.0f, 1.0f);
					vkCmdSetViewport(drawCmdBuffers[i], 0, 1, &viewport);

					VkRect2D scissor = vks::initializers::rect2D(width, height, 0, 0);
					vkCmdSetScissor(drawCmdBuffers[i], 0, 1, &scissor);

					// Skybox
					if (displaySkybox)
					{
						vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSets.skybox, 0, NULL);
						vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.skybox);
						models.skybox.draw(drawCmdBuffers[i]);
					}

					// Objects
					vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSets.object, 0, NULL);
					vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.pbr);

					// Render a line of objects with the selected material and vary roughness/metallic material parameters
					Material mat = materials[materialIndex];
					const uint32_t objcount = 10;
					for (uint32_t x = 0; x < objcount; x++) {
						glm::vec3 pos = glm::vec3(float(x - (objcount / 2.0f)) * 2.15f, 0.0f, 0.0f);
						mat.params.roughness = 1.0f - glm::clamp((float)x / (float)objcount, 0.005f, 1.0f);
						mat.params.metallic = glm::clamp((float)x / (float)objcount, 0.005f, 1.0f);

						PushConstStruct pcs;
						pcs.pos = pos;
						pcs.offset = frame_buffer_index;

						vkCmdPushConstants(drawCmdBuffers[i], pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstStruct), &pcs);
						vkCmdPushConstants(drawCmdBuffers[i], pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(PushConstStruct), sizeof(Material::PushBlock), &mat);
						models.objects[models.objectIndex].draw(drawCmdBuffers[i]);
					}
				}

				//drawUI(drawCmdBuffers[i]);

				vkCmdEndRenderPass(drawCmdBuffers[i]);
			}

			{
				if (true)
				{
					float sub_viewport_width = (width * 1.0) / column;
					float sub_viewport_height = (height * 1.0) / row;

					uint32_t current_view_index = 0;
					for (size_t multi_view_color_attachment_index = 0; multi_view_color_attachment_index < multiviewPass.colors.size(); multi_view_color_attachment_index++)
					{
						auto color_attachment_ref = multiviewPass.colors[multi_view_color_attachment_index];
						auto used_array_layers = color_attachment_ref.usedArrayLayers;
						VkImage blit_src_image = color_attachment_ref.image;

						std::vector<VkImageBlit>sub_image_blits;
						{
							for (size_t used_array_layer = 0; used_array_layer < used_array_layers; used_array_layer++)
							{
								uint32_t row_index = current_view_index / column;
								uint32_t column_index = current_view_index - row_index * column;

								float sub_viewport_x = column_index * sub_viewport_width;
								float sub_viewport_y = row_index * sub_viewport_height;

								{
									VkImageBlit sub_image_blit = {};
									{
										sub_image_blit.srcSubresource.aspectMask = VkImageAspectFlagBits::VK_IMAGE_ASPECT_COLOR_BIT;
										sub_image_blit.srcSubresource.mipLevel = 0;
										sub_image_blit.srcSubresource.baseArrayLayer = used_array_layer;
										sub_image_blit.srcSubresource.layerCount = 1;

										sub_image_blit.srcOffsets[0].x = 0;
										sub_image_blit.srcOffsets[0].y = 0;
										sub_image_blit.srcOffsets[0].z = 0;
										sub_image_blit.srcOffsets[1].x = width;
										sub_image_blit.srcOffsets[1].y = height;
										sub_image_blit.srcOffsets[1].z = 1;

										sub_image_blit.dstSubresource.aspectMask = VkImageAspectFlagBits::VK_IMAGE_ASPECT_COLOR_BIT;
										sub_image_blit.dstSubresource.mipLevel = 0;
										sub_image_blit.dstSubresource.baseArrayLayer = 0;
										sub_image_blit.dstSubresource.layerCount = 1;

										sub_image_blit.dstOffsets[0].x = sub_viewport_x;
										sub_image_blit.dstOffsets[0].y = sub_viewport_y;
										sub_image_blit.dstOffsets[0].z = 0;
										sub_image_blit.dstOffsets[1].x = sub_image_blit.dstOffsets[0].x + sub_viewport_width;
										sub_image_blit.dstOffsets[1].y = sub_image_blit.dstOffsets[0].y + sub_viewport_height;
										sub_image_blit.dstOffsets[1].z = 1;
									}

									sub_image_blits.push_back(sub_image_blit);
								}

								{
									current_view_index += 1;
								}
							}
						}

						vkCmdBlitImage(
							drawCmdBuffers[i],
							blit_src_image,
							VkImageLayout::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
							swapChain.buffers[i].image,
							VkImageLayout::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							sub_image_blits.size(),
							sub_image_blits.data(),
							VkFilter::VK_FILTER_LINEAR
						);
					}
				}

				if (false)
				{
					std::vector<VkImageBlit>sub_image_blits;
					{
						VkImageBlit image_blit = {};
						image_blit.srcSubresource.aspectMask = VkImageAspectFlagBits::VK_IMAGE_ASPECT_COLOR_BIT;
						image_blit.srcSubresource.mipLevel = 0;
						image_blit.srcSubresource.baseArrayLayer = 0;
						image_blit.srcSubresource.layerCount = 1;

						image_blit.srcOffsets[0].x = 0;
						image_blit.srcOffsets[0].y = 0;
						image_blit.srcOffsets[0].z = 0;
						image_blit.srcOffsets[1].x = width;
						image_blit.srcOffsets[1].y = height;
						image_blit.srcOffsets[1].z = 1;

						image_blit.dstSubresource.aspectMask = VkImageAspectFlagBits::VK_IMAGE_ASPECT_COLOR_BIT;
						image_blit.dstSubresource.mipLevel = 0;
						image_blit.dstSubresource.baseArrayLayer = 0;
						image_blit.dstSubresource.layerCount = 1;

						image_blit.dstOffsets[0].x = 0;
						image_blit.dstOffsets[0].y = 0;
						image_blit.dstOffsets[0].z = 0;
						image_blit.dstOffsets[1].x = width * 0.5;
						image_blit.dstOffsets[1].y = height * 0.5;
						image_blit.dstOffsets[1].z = 1;

						sub_image_blits.push_back(image_blit);

						image_blit.srcSubresource.baseArrayLayer = 1;
						image_blit.dstOffsets[0].x = 100;
						image_blit.dstOffsets[0].y = 100;
						image_blit.dstOffsets[1].x = image_blit.dstOffsets[0].x + width * 0.5;
						image_blit.dstOffsets[1].y = image_blit.dstOffsets[0].y + height * 0.5;
						sub_image_blits.push_back(image_blit);

						image_blit.srcSubresource.baseArrayLayer = 2;
						image_blit.dstOffsets[0].x = 200;
						image_blit.dstOffsets[0].y = 200;
						image_blit.dstOffsets[1].x = image_blit.dstOffsets[0].x + width * 0.5;
						image_blit.dstOffsets[1].y = image_blit.dstOffsets[0].y + height * 0.5;
						sub_image_blits.push_back(image_blit);
					}

					vkCmdBlitImage(
						drawCmdBuffers[i],
						multiviewPass.colors[0].image,
						VkImageLayout::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
						swapChain.buffers[i].image,
						VkImageLayout::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						sub_image_blits.size(),
						sub_image_blits.data(),
						VkFilter::VK_FILTER_LINEAR
					);
				}
			}

			/*
				ImGui
			*/
			{
				VkClearValue clearValues[2];
				clearValues[0].color = { { 0.1f, 0.1f, 0.1f, 1.0f } };
				clearValues[1].depthStencil = { 1.0f, 0 };

				VkRenderPassBeginInfo imgui_renderPassBeginInfo = vks::initializers::renderPassBeginInfo();
				//renderPassBeginInfo.renderPass = renderPass;
				imgui_renderPassBeginInfo.renderPass = imGuiPass.renderPass;
				imgui_renderPassBeginInfo.renderArea.offset.x = 0;
				imgui_renderPassBeginInfo.renderArea.offset.y = 0;
				imgui_renderPassBeginInfo.renderArea.extent.width = width;
				imgui_renderPassBeginInfo.renderArea.extent.height = height;
				imgui_renderPassBeginInfo.clearValueCount = 2;
				imgui_renderPassBeginInfo.pClearValues = clearValues;
				//imgui_renderPassBeginInfo.clearValueCount = 0;
				//imgui_renderPassBeginInfo.pClearValues = nullptr;
				imgui_renderPassBeginInfo.framebuffer = frameBuffers[i];

				vkCmdBeginRenderPass(drawCmdBuffers[i], &imgui_renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

				VkViewport viewport = vks::initializers::viewport((float)width, (float)height, 0.0f, 1.0f);
				vkCmdSetViewport(drawCmdBuffers[i], 0, 1, &viewport);

				VkRect2D scissor = vks::initializers::rect2D(width, height, 0, 0);
				vkCmdSetScissor(drawCmdBuffers[i], 0, 1, &scissor);

				drawUI(drawCmdBuffers[i]);

				vkCmdEndRenderPass(drawCmdBuffers[i]);
			}

			VK_CHECK_RESULT(vkEndCommandBuffer(drawCmdBuffers[i]));
		}
	}

	void loadAssets()
	{
		uint32_t glTFLoadingFlags = vkglTF::FileLoadingFlags::PreTransformVertices | vkglTF::FileLoadingFlags::FlipY;
		// Skybox
		models.skybox.loadFromFile(getAssetPath() + "models/cube.gltf", vulkanDevice, queue, glTFLoadingFlags);
		// Objects
		std::vector<std::string> filenames = { "sphere.gltf", "teapot.gltf", "torusknot.gltf", "venus.gltf" ,"deer.gltf" };
		models.objects.resize(filenames.size());
		for (size_t i = 0; i < filenames.size(); i++) {
			models.objects[i].loadFromFile(getAssetPath() + "models/" + filenames[i], vulkanDevice, queue, glTFLoadingFlags);
		}
		// HDR cubemap
		textures.environmentCube.loadFromFile(getAssetPath() + "textures/hdr/pisa_cube.ktx", VK_FORMAT_R16G16B16A16_SFLOAT, vulkanDevice, queue);
	}

	void setupDescriptors()
	{
		// Descriptor Pool
		std::vector<VkDescriptorPoolSize> poolSizes = {
			vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 10),
			vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,10)
		};
		VkDescriptorPoolCreateInfo descriptorPoolInfo = vks::initializers::descriptorPoolCreateInfo(poolSizes, 2);
		VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr, &descriptorPool));

		// Descriptor set layout
		std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings = {
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0),
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT, 1),
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 2),
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 3),
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 4),
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 5),
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT, 6),
		};
		VkDescriptorSetLayoutCreateInfo descriptorLayout = vks::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings);
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorLayout, nullptr, &descriptorSetLayout));

		// Descriptor sets
		VkDescriptorSetAllocateInfo allocInfo = vks::initializers::descriptorSetAllocateInfo(descriptorPool, &descriptorSetLayout, 1);

		VkDescriptorImageInfo diffuse_descriptor_image_info = {};
		diffuse_descriptor_image_info.sampler = textures.lutBrdf.descriptor.sampler;
		diffuse_descriptor_image_info.imageView = models.objects.back().textures.front().view;
		diffuse_descriptor_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		// Objects
		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &descriptorSets.object));
		std::vector<VkWriteDescriptorSet> writeDescriptorSets = {
			vks::initializers::writeDescriptorSet(descriptorSets.object, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, &uniformBuffers.object.descriptor),
			vks::initializers::writeDescriptorSet(descriptorSets.object, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, &uniformBuffers.params.descriptor),
			vks::initializers::writeDescriptorSet(descriptorSets.object, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, &textures.irradianceCube.descriptor),
			vks::initializers::writeDescriptorSet(descriptorSets.object, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3, &textures.lutBrdf.descriptor),
			vks::initializers::writeDescriptorSet(descriptorSets.object, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4, &textures.prefilteredCube.descriptor),
			vks::initializers::writeDescriptorSet(descriptorSets.object, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5, &diffuse_descriptor_image_info),
			vks::initializers::writeDescriptorSet(descriptorSets.object, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 6, &uniformBuffers.views.descriptor),
		};
		vkUpdateDescriptorSets(device, static_cast<uint32_t>(writeDescriptorSets.size()), writeDescriptorSets.data(), 0, NULL);

		// Sky box
		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &descriptorSets.skybox));
		writeDescriptorSets = {
			vks::initializers::writeDescriptorSet(descriptorSets.skybox, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, &uniformBuffers.skybox.descriptor),
			vks::initializers::writeDescriptorSet(descriptorSets.skybox, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, &uniformBuffers.params.descriptor),
			vks::initializers::writeDescriptorSet(descriptorSets.skybox, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, &textures.environmentCube.descriptor),
		};
		vkUpdateDescriptorSets(device, static_cast<uint32_t>(writeDescriptorSets.size()), writeDescriptorSets.data(), 0, NULL);
	}

	void preparePipelines()
	{
		VkPipelineInputAssemblyStateCreateInfo inputAssemblyState =
			vks::initializers::pipelineInputAssemblyStateCreateInfo(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE);

		VkPipelineRasterizationStateCreateInfo rasterizationState =
			vks::initializers::pipelineRasterizationStateCreateInfo(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE);

		VkPipelineColorBlendAttachmentState blendAttachmentState =
			vks::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE);

		VkPipelineColorBlendStateCreateInfo colorBlendState =
			vks::initializers::pipelineColorBlendStateCreateInfo(1, &blendAttachmentState);

		VkPipelineDepthStencilStateCreateInfo depthStencilState =
			vks::initializers::pipelineDepthStencilStateCreateInfo(VK_FALSE, VK_FALSE, VK_COMPARE_OP_LESS_OR_EQUAL);

		VkPipelineViewportStateCreateInfo viewportState =
			vks::initializers::pipelineViewportStateCreateInfo(1, 1);

		VkPipelineMultisampleStateCreateInfo multisampleState =
			vks::initializers::pipelineMultisampleStateCreateInfo(VK_SAMPLE_COUNT_1_BIT);

		std::vector<VkDynamicState> dynamicStateEnables = {
			VK_DYNAMIC_STATE_VIEWPORT,
			VK_DYNAMIC_STATE_SCISSOR
		};
		VkPipelineDynamicStateCreateInfo dynamicState =
			vks::initializers::pipelineDynamicStateCreateInfo(dynamicStateEnables);

		// Pipeline layout
		VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = vks::initializers::pipelineLayoutCreateInfo(&descriptorSetLayout, 1);
		// Push constant ranges
		std::vector<VkPushConstantRange> pushConstantRanges = {
			vks::initializers::pushConstantRange(VK_SHADER_STAGE_VERTEX_BIT, sizeof(PushConstStruct), 0),
			vks::initializers::pushConstantRange(VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(Material::PushBlock), sizeof(PushConstStruct)),
		};
		pipelineLayoutCreateInfo.pushConstantRangeCount = 2;
		pipelineLayoutCreateInfo.pPushConstantRanges = pushConstantRanges.data();
		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &pipelineLayout));

		std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages;

		// Pipelines
		VkGraphicsPipelineCreateInfo pipelineCI = vks::initializers::pipelineCreateInfo(pipelineLayout, renderPass);
		pipelineCI.pInputAssemblyState = &inputAssemblyState;
		pipelineCI.pRasterizationState = &rasterizationState;
		pipelineCI.pColorBlendState = &colorBlendState;
		pipelineCI.pMultisampleState = &multisampleState;
		pipelineCI.pViewportState = &viewportState;
		pipelineCI.pDepthStencilState = &depthStencilState;
		pipelineCI.pDynamicState = &dynamicState;
		pipelineCI.stageCount = static_cast<uint32_t>(shaderStages.size());
		pipelineCI.pStages = shaderStages.data();
		pipelineCI.pVertexInputState = vkglTF::Vertex::getPipelineVertexInputState({ vkglTF::VertexComponent::Position, vkglTF::VertexComponent::Normal, vkglTF::VertexComponent::UV });

		// Skybox pipeline (background cube)
		shaderStages[0] = loadShader(getShadersPath() + "pbribl/skybox.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		shaderStages[1] = loadShader(getShadersPath() + "pbribl/skybox.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.skybox));

		// PBR pipeline
		shaderStages[0] = loadShader(getShadersPath() + "pbribl/pbribl_mv.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		shaderStages[1] = loadShader(getShadersPath() + "pbribl/pbribl_mv.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
		// Enable depth test and write
		depthStencilState.depthWriteEnable = VK_TRUE;
		depthStencilState.depthTestEnable = VK_TRUE;
		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.pbr));
	}

	// Generate a BRDF integration map used as a look-up-table (stores roughness / NdotV)
	void generateBRDFLUT()
	{
		auto tStart = std::chrono::high_resolution_clock::now();

		const VkFormat format = VK_FORMAT_R16G16_SFLOAT;	// R16G16 is supported pretty much everywhere
		const int32_t dim = 512;

		// Image
		VkImageCreateInfo imageCI = vks::initializers::imageCreateInfo();
		imageCI.imageType = VK_IMAGE_TYPE_2D;
		imageCI.format = format;
		imageCI.extent.width = dim;
		imageCI.extent.height = dim;
		imageCI.extent.depth = 1;
		imageCI.mipLevels = 1;
		imageCI.arrayLayers = 1;
		imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
		imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageCI.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		VK_CHECK_RESULT(vkCreateImage(device, &imageCI, nullptr, &textures.lutBrdf.image));
		VkMemoryAllocateInfo memAlloc = vks::initializers::memoryAllocateInfo();
		VkMemoryRequirements memReqs;
		vkGetImageMemoryRequirements(device, textures.lutBrdf.image, &memReqs);
		memAlloc.allocationSize = memReqs.size;
		memAlloc.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &textures.lutBrdf.deviceMemory));
		VK_CHECK_RESULT(vkBindImageMemory(device, textures.lutBrdf.image, textures.lutBrdf.deviceMemory, 0));
		// Image view
		VkImageViewCreateInfo viewCI = vks::initializers::imageViewCreateInfo();
		viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewCI.format = format;
		viewCI.subresourceRange = {};
		viewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewCI.subresourceRange.levelCount = 1;
		viewCI.subresourceRange.layerCount = 1;
		viewCI.image = textures.lutBrdf.image;
		VK_CHECK_RESULT(vkCreateImageView(device, &viewCI, nullptr, &textures.lutBrdf.view));
		// Sampler
		VkSamplerCreateInfo samplerCI = vks::initializers::samplerCreateInfo();
		samplerCI.magFilter = VK_FILTER_LINEAR;
		samplerCI.minFilter = VK_FILTER_LINEAR;
		samplerCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerCI.minLod = 0.0f;
		samplerCI.maxLod = 1.0f;
		samplerCI.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
		VK_CHECK_RESULT(vkCreateSampler(device, &samplerCI, nullptr, &textures.lutBrdf.sampler));

		textures.lutBrdf.descriptor.imageView = textures.lutBrdf.view;
		textures.lutBrdf.descriptor.sampler = textures.lutBrdf.sampler;
		textures.lutBrdf.descriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		textures.lutBrdf.device = vulkanDevice;

		// FB, Att, RP, Pipe, etc.
		VkAttachmentDescription attDesc = {};
		// Color attachment
		attDesc.format = format;
		attDesc.samples = VK_SAMPLE_COUNT_1_BIT;
		attDesc.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attDesc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attDesc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attDesc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attDesc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attDesc.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		VkAttachmentReference colorReference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

		VkSubpassDescription subpassDescription = {};
		subpassDescription.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpassDescription.colorAttachmentCount = 1;
		subpassDescription.pColorAttachments = &colorReference;

		// Use subpass dependencies for layout transitions
		std::array<VkSubpassDependency, 2> dependencies;
		dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
		dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
		dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
		dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
		dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

		// Create the actual renderpass
		VkRenderPassCreateInfo renderPassCI = vks::initializers::renderPassCreateInfo();
		renderPassCI.attachmentCount = 1;
		renderPassCI.pAttachments = &attDesc;
		renderPassCI.subpassCount = 1;
		renderPassCI.pSubpasses = &subpassDescription;
		renderPassCI.dependencyCount = 2;
		renderPassCI.pDependencies = dependencies.data();

		VkRenderPass renderpass;
		VK_CHECK_RESULT(vkCreateRenderPass(device, &renderPassCI, nullptr, &renderpass));

		VkFramebufferCreateInfo framebufferCI = vks::initializers::framebufferCreateInfo();
		framebufferCI.renderPass = renderpass;
		framebufferCI.attachmentCount = 1;
		framebufferCI.pAttachments = &textures.lutBrdf.view;
		framebufferCI.width = dim;
		framebufferCI.height = dim;
		framebufferCI.layers = 1;

		VkFramebuffer framebuffer;
		VK_CHECK_RESULT(vkCreateFramebuffer(device, &framebufferCI, nullptr, &framebuffer));

		// Descriptors
		VkDescriptorSetLayout descriptorsetlayout;
		std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings = {};
		VkDescriptorSetLayoutCreateInfo descriptorsetlayoutCI = vks::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings);
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorsetlayoutCI, nullptr, &descriptorsetlayout));

		// Descriptor Pool
		std::vector<VkDescriptorPoolSize> poolSizes = { vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1) };
		VkDescriptorPoolCreateInfo descriptorPoolCI = vks::initializers::descriptorPoolCreateInfo(poolSizes, 2);
		VkDescriptorPool descriptorpool;
		VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolCI, nullptr, &descriptorpool));

		// Descriptor sets
		VkDescriptorSet descriptorset;
		VkDescriptorSetAllocateInfo allocInfo = vks::initializers::descriptorSetAllocateInfo(descriptorpool, &descriptorsetlayout, 1);
		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &descriptorset));

		// Pipeline layout
		VkPipelineLayout pipelinelayout;
		VkPipelineLayoutCreateInfo pipelineLayoutCI = vks::initializers::pipelineLayoutCreateInfo(&descriptorsetlayout, 1);
		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCI, nullptr, &pipelinelayout));

		// Pipeline
		VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = vks::initializers::pipelineInputAssemblyStateCreateInfo(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE);
		VkPipelineRasterizationStateCreateInfo rasterizationState = vks::initializers::pipelineRasterizationStateCreateInfo(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE);
		VkPipelineColorBlendAttachmentState blendAttachmentState = vks::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE);
		VkPipelineColorBlendStateCreateInfo colorBlendState = vks::initializers::pipelineColorBlendStateCreateInfo(1, &blendAttachmentState);
		VkPipelineDepthStencilStateCreateInfo depthStencilState = vks::initializers::pipelineDepthStencilStateCreateInfo(VK_FALSE, VK_FALSE, VK_COMPARE_OP_LESS_OR_EQUAL);
		VkPipelineViewportStateCreateInfo viewportState = vks::initializers::pipelineViewportStateCreateInfo(1, 1);
		VkPipelineMultisampleStateCreateInfo multisampleState = vks::initializers::pipelineMultisampleStateCreateInfo(VK_SAMPLE_COUNT_1_BIT);
		std::vector<VkDynamicState> dynamicStateEnables = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamicState = vks::initializers::pipelineDynamicStateCreateInfo(dynamicStateEnables);
		VkPipelineVertexInputStateCreateInfo emptyInputState = vks::initializers::pipelineVertexInputStateCreateInfo();
		std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages;

		VkGraphicsPipelineCreateInfo pipelineCI = vks::initializers::pipelineCreateInfo(pipelinelayout, renderpass);
		pipelineCI.pInputAssemblyState = &inputAssemblyState;
		pipelineCI.pRasterizationState = &rasterizationState;
		pipelineCI.pColorBlendState = &colorBlendState;
		pipelineCI.pMultisampleState = &multisampleState;
		pipelineCI.pViewportState = &viewportState;
		pipelineCI.pDepthStencilState = &depthStencilState;
		pipelineCI.pDynamicState = &dynamicState;
		pipelineCI.stageCount = 2;
		pipelineCI.pStages = shaderStages.data();
		pipelineCI.pVertexInputState = &emptyInputState;

		// Look-up-table (from BRDF) pipeline
		shaderStages[0] = loadShader(getShadersPath() + "pbribl/genbrdflut.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		shaderStages[1] = loadShader(getShadersPath() + "pbribl/genbrdflut.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
		VkPipeline pipeline;
		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipeline));

		// Render
		VkClearValue clearValues[1];
		clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };

		VkRenderPassBeginInfo renderPassBeginInfo = vks::initializers::renderPassBeginInfo();
		renderPassBeginInfo.renderPass = renderpass;
		renderPassBeginInfo.renderArea.extent.width = dim;
		renderPassBeginInfo.renderArea.extent.height = dim;
		renderPassBeginInfo.clearValueCount = 1;
		renderPassBeginInfo.pClearValues = clearValues;
		renderPassBeginInfo.framebuffer = framebuffer;

		VkCommandBuffer cmdBuf = vulkanDevice->createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);
		vkCmdBeginRenderPass(cmdBuf, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
		VkViewport viewport = vks::initializers::viewport((float)dim, (float)dim, 0.0f, 1.0f);
		VkRect2D scissor = vks::initializers::rect2D(dim, dim, 0, 0);
		vkCmdSetViewport(cmdBuf, 0, 1, &viewport);
		vkCmdSetScissor(cmdBuf, 0, 1, &scissor);
		vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdDraw(cmdBuf, 3, 1, 0, 0);
		vkCmdEndRenderPass(cmdBuf);
		vulkanDevice->flushCommandBuffer(cmdBuf, queue);

		vkQueueWaitIdle(queue);

		vkDestroyPipeline(device, pipeline, nullptr);
		vkDestroyPipelineLayout(device, pipelinelayout, nullptr);
		vkDestroyRenderPass(device, renderpass, nullptr);
		vkDestroyFramebuffer(device, framebuffer, nullptr);
		vkDestroyDescriptorSetLayout(device, descriptorsetlayout, nullptr);
		vkDestroyDescriptorPool(device, descriptorpool, nullptr);

		auto tEnd = std::chrono::high_resolution_clock::now();
		auto tDiff = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
		std::cout << "Generating BRDF LUT took " << tDiff << " ms" << std::endl;
	}

	// Generate an irradiance cube map from the environment cube map
	void generateIrradianceCube()
	{
		auto tStart = std::chrono::high_resolution_clock::now();

		const VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
		const int32_t dim = 64;
		const uint32_t numMips = static_cast<uint32_t>(floor(log2(dim))) + 1;

		// Pre-filtered cube map
		// Image
		VkImageCreateInfo imageCI = vks::initializers::imageCreateInfo();
		imageCI.imageType = VK_IMAGE_TYPE_2D;
		imageCI.format = format;
		imageCI.extent.width = dim;
		imageCI.extent.height = dim;
		imageCI.extent.depth = 1;
		imageCI.mipLevels = numMips;
		imageCI.arrayLayers = 6;
		imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
		imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageCI.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		imageCI.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
		VK_CHECK_RESULT(vkCreateImage(device, &imageCI, nullptr, &textures.irradianceCube.image));
		VkMemoryAllocateInfo memAlloc = vks::initializers::memoryAllocateInfo();
		VkMemoryRequirements memReqs;
		vkGetImageMemoryRequirements(device, textures.irradianceCube.image, &memReqs);
		memAlloc.allocationSize = memReqs.size;
		memAlloc.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &textures.irradianceCube.deviceMemory));
		VK_CHECK_RESULT(vkBindImageMemory(device, textures.irradianceCube.image, textures.irradianceCube.deviceMemory, 0));
		// Image view
		VkImageViewCreateInfo viewCI = vks::initializers::imageViewCreateInfo();
		viewCI.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
		viewCI.format = format;
		viewCI.subresourceRange = {};
		viewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewCI.subresourceRange.levelCount = numMips;
		viewCI.subresourceRange.layerCount = 6;
		viewCI.image = textures.irradianceCube.image;
		VK_CHECK_RESULT(vkCreateImageView(device, &viewCI, nullptr, &textures.irradianceCube.view));
		// Sampler
		VkSamplerCreateInfo samplerCI = vks::initializers::samplerCreateInfo();
		samplerCI.magFilter = VK_FILTER_LINEAR;
		samplerCI.minFilter = VK_FILTER_LINEAR;
		samplerCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerCI.minLod = 0.0f;
		samplerCI.maxLod = static_cast<float>(numMips);
		samplerCI.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
		VK_CHECK_RESULT(vkCreateSampler(device, &samplerCI, nullptr, &textures.irradianceCube.sampler));

		textures.irradianceCube.descriptor.imageView = textures.irradianceCube.view;
		textures.irradianceCube.descriptor.sampler = textures.irradianceCube.sampler;
		textures.irradianceCube.descriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		textures.irradianceCube.device = vulkanDevice;

		// FB, Att, RP, Pipe, etc.
		VkAttachmentDescription attDesc = {};
		// Color attachment
		attDesc.format = format;
		attDesc.samples = VK_SAMPLE_COUNT_1_BIT;
		attDesc.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attDesc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attDesc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attDesc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attDesc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attDesc.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		VkAttachmentReference colorReference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

		VkSubpassDescription subpassDescription = {};
		subpassDescription.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpassDescription.colorAttachmentCount = 1;
		subpassDescription.pColorAttachments = &colorReference;

		// Use subpass dependencies for layout transitions
		std::array<VkSubpassDependency, 2> dependencies;
		dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
		dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
		dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
		dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
		dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

		// Renderpass
		VkRenderPassCreateInfo renderPassCI = vks::initializers::renderPassCreateInfo();
		renderPassCI.attachmentCount = 1;
		renderPassCI.pAttachments = &attDesc;
		renderPassCI.subpassCount = 1;
		renderPassCI.pSubpasses = &subpassDescription;
		renderPassCI.dependencyCount = 2;
		renderPassCI.pDependencies = dependencies.data();
		VkRenderPass renderpass;
		VK_CHECK_RESULT(vkCreateRenderPass(device, &renderPassCI, nullptr, &renderpass));

		struct {
			VkImage image;
			VkImageView view;
			VkDeviceMemory memory;
			VkFramebuffer framebuffer;
		} offscreen;

		// Offscreen framebuffer
		{
			// Color attachment
			VkImageCreateInfo imageCreateInfo = vks::initializers::imageCreateInfo();
			imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
			imageCreateInfo.format = format;
			imageCreateInfo.extent.width = dim;
			imageCreateInfo.extent.height = dim;
			imageCreateInfo.extent.depth = 1;
			imageCreateInfo.mipLevels = 1;
			imageCreateInfo.arrayLayers = 1;
			imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
			imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
			imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			imageCreateInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VK_CHECK_RESULT(vkCreateImage(device, &imageCreateInfo, nullptr, &offscreen.image));

			VkMemoryAllocateInfo memAlloc = vks::initializers::memoryAllocateInfo();
			VkMemoryRequirements memReqs;
			vkGetImageMemoryRequirements(device, offscreen.image, &memReqs);
			memAlloc.allocationSize = memReqs.size;
			memAlloc.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &offscreen.memory));
			VK_CHECK_RESULT(vkBindImageMemory(device, offscreen.image, offscreen.memory, 0));

			VkImageViewCreateInfo colorImageView = vks::initializers::imageViewCreateInfo();
			colorImageView.viewType = VK_IMAGE_VIEW_TYPE_2D;
			colorImageView.format = format;
			colorImageView.flags = 0;
			colorImageView.subresourceRange = {};
			colorImageView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			colorImageView.subresourceRange.baseMipLevel = 0;
			colorImageView.subresourceRange.levelCount = 1;
			colorImageView.subresourceRange.baseArrayLayer = 0;
			colorImageView.subresourceRange.layerCount = 1;
			colorImageView.image = offscreen.image;
			VK_CHECK_RESULT(vkCreateImageView(device, &colorImageView, nullptr, &offscreen.view));

			VkFramebufferCreateInfo fbufCreateInfo = vks::initializers::framebufferCreateInfo();
			fbufCreateInfo.renderPass = renderpass;
			fbufCreateInfo.attachmentCount = 1;
			fbufCreateInfo.pAttachments = &offscreen.view;
			fbufCreateInfo.width = dim;
			fbufCreateInfo.height = dim;
			fbufCreateInfo.layers = 1;
			VK_CHECK_RESULT(vkCreateFramebuffer(device, &fbufCreateInfo, nullptr, &offscreen.framebuffer));

			VkCommandBuffer layoutCmd = vulkanDevice->createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);
			vks::tools::setImageLayout(
				layoutCmd,
				offscreen.image,
				VK_IMAGE_ASPECT_COLOR_BIT,
				VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
			vulkanDevice->flushCommandBuffer(layoutCmd, queue, true);
		}

		// Descriptors
		VkDescriptorSetLayout descriptorsetlayout;
		std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings = {
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 0),
		};
		VkDescriptorSetLayoutCreateInfo descriptorsetlayoutCI = vks::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings);
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorsetlayoutCI, nullptr, &descriptorsetlayout));

		// Descriptor Pool
		std::vector<VkDescriptorPoolSize> poolSizes = { vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1) };
		VkDescriptorPoolCreateInfo descriptorPoolCI = vks::initializers::descriptorPoolCreateInfo(poolSizes, 2);
		VkDescriptorPool descriptorpool;
		VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolCI, nullptr, &descriptorpool));

		// Descriptor sets
		VkDescriptorSet descriptorset;
		VkDescriptorSetAllocateInfo allocInfo = vks::initializers::descriptorSetAllocateInfo(descriptorpool, &descriptorsetlayout, 1);
		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &descriptorset));
		VkWriteDescriptorSet writeDescriptorSet = vks::initializers::writeDescriptorSet(descriptorset, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0, &textures.environmentCube.descriptor);
		vkUpdateDescriptorSets(device, 1, &writeDescriptorSet, 0, nullptr);

		// Pipeline layout
		struct PushBlock {
			glm::mat4 mvp;
			// Sampling deltas
			float deltaPhi = (2.0f * float(M_PI)) / 180.0f;
			float deltaTheta = (0.5f * float(M_PI)) / 64.0f;
		} pushBlock;

		VkPipelineLayout pipelinelayout;
		std::vector<VkPushConstantRange> pushConstantRanges = {
			vks::initializers::pushConstantRange(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(PushBlock), 0),
		};
		VkPipelineLayoutCreateInfo pipelineLayoutCI = vks::initializers::pipelineLayoutCreateInfo(&descriptorsetlayout, 1);
		pipelineLayoutCI.pushConstantRangeCount = 1;
		pipelineLayoutCI.pPushConstantRanges = pushConstantRanges.data();
		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCI, nullptr, &pipelinelayout));

		// Pipeline
		VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = vks::initializers::pipelineInputAssemblyStateCreateInfo(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE);
		VkPipelineRasterizationStateCreateInfo rasterizationState = vks::initializers::pipelineRasterizationStateCreateInfo(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE);
		VkPipelineColorBlendAttachmentState blendAttachmentState = vks::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE);
		VkPipelineColorBlendStateCreateInfo colorBlendState = vks::initializers::pipelineColorBlendStateCreateInfo(1, &blendAttachmentState);
		VkPipelineDepthStencilStateCreateInfo depthStencilState = vks::initializers::pipelineDepthStencilStateCreateInfo(VK_FALSE, VK_FALSE, VK_COMPARE_OP_LESS_OR_EQUAL);
		VkPipelineViewportStateCreateInfo viewportState = vks::initializers::pipelineViewportStateCreateInfo(1, 1);
		VkPipelineMultisampleStateCreateInfo multisampleState = vks::initializers::pipelineMultisampleStateCreateInfo(VK_SAMPLE_COUNT_1_BIT);
		std::vector<VkDynamicState> dynamicStateEnables = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamicState = vks::initializers::pipelineDynamicStateCreateInfo(dynamicStateEnables);
		std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages;

		VkGraphicsPipelineCreateInfo pipelineCI = vks::initializers::pipelineCreateInfo(pipelinelayout, renderpass);
		pipelineCI.pInputAssemblyState = &inputAssemblyState;
		pipelineCI.pRasterizationState = &rasterizationState;
		pipelineCI.pColorBlendState = &colorBlendState;
		pipelineCI.pMultisampleState = &multisampleState;
		pipelineCI.pViewportState = &viewportState;
		pipelineCI.pDepthStencilState = &depthStencilState;
		pipelineCI.pDynamicState = &dynamicState;
		pipelineCI.stageCount = 2;
		pipelineCI.pStages = shaderStages.data();
		pipelineCI.renderPass = renderpass;
		pipelineCI.pVertexInputState = vkglTF::Vertex::getPipelineVertexInputState({ vkglTF::VertexComponent::Position, vkglTF::VertexComponent::Normal, vkglTF::VertexComponent::UV });

		shaderStages[0] = loadShader(getShadersPath() + "pbribl/filtercube.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		shaderStages[1] = loadShader(getShadersPath() + "pbribl/irradiancecube.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
		VkPipeline pipeline;
		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipeline));

		// Render

		VkClearValue clearValues[1];
		clearValues[0].color = { { 0.0f, 0.0f, 0.2f, 0.0f } };

		VkRenderPassBeginInfo renderPassBeginInfo = vks::initializers::renderPassBeginInfo();
		// Reuse render pass from example pass
		renderPassBeginInfo.renderPass = renderpass;
		renderPassBeginInfo.framebuffer = offscreen.framebuffer;
		renderPassBeginInfo.renderArea.extent.width = dim;
		renderPassBeginInfo.renderArea.extent.height = dim;
		renderPassBeginInfo.clearValueCount = 1;
		renderPassBeginInfo.pClearValues = clearValues;

		std::vector<glm::mat4> matrices = {
			// POSITIVE_X
			glm::rotate(glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f)), glm::radians(180.0f), glm::vec3(1.0f, 0.0f, 0.0f)),
			// NEGATIVE_X
			glm::rotate(glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(0.0f, 1.0f, 0.0f)), glm::radians(180.0f), glm::vec3(1.0f, 0.0f, 0.0f)),
			// POSITIVE_Y
			glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f)),
			// NEGATIVE_Y
			glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f)),
			// POSITIVE_Z
			glm::rotate(glm::mat4(1.0f), glm::radians(180.0f), glm::vec3(1.0f, 0.0f, 0.0f)),
			// NEGATIVE_Z
			glm::rotate(glm::mat4(1.0f), glm::radians(180.0f), glm::vec3(0.0f, 0.0f, 1.0f)),
		};

		VkCommandBuffer cmdBuf = vulkanDevice->createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);

		VkViewport viewport = vks::initializers::viewport((float)dim, (float)dim, 0.0f, 1.0f);
		VkRect2D scissor = vks::initializers::rect2D(dim, dim, 0, 0);

		vkCmdSetViewport(cmdBuf, 0, 1, &viewport);
		vkCmdSetScissor(cmdBuf, 0, 1, &scissor);

		VkImageSubresourceRange subresourceRange = {};
		subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		subresourceRange.baseMipLevel = 0;
		subresourceRange.levelCount = numMips;
		subresourceRange.layerCount = 6;

		// Change image layout for all cubemap faces to transfer destination
		vks::tools::setImageLayout(
			cmdBuf,
			textures.irradianceCube.image,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			subresourceRange);

		for (uint32_t m = 0; m < numMips; m++) {
			for (uint32_t f = 0; f < 6; f++) {
				viewport.width = static_cast<float>(dim * std::pow(0.5f, m));
				viewport.height = static_cast<float>(dim * std::pow(0.5f, m));
				vkCmdSetViewport(cmdBuf, 0, 1, &viewport);

				// Render scene from cube face's point of view
				vkCmdBeginRenderPass(cmdBuf, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

				// Update shader push constant block
				pushBlock.mvp = glm::perspective((float)(M_PI / 2.0), 1.0f, 0.1f, 512.0f) * matrices[f];

				vkCmdPushConstants(cmdBuf, pipelinelayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushBlock), &pushBlock);

				vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
				vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelinelayout, 0, 1, &descriptorset, 0, NULL);

				models.skybox.draw(cmdBuf);

				vkCmdEndRenderPass(cmdBuf);

				vks::tools::setImageLayout(
					cmdBuf,
					offscreen.image,
					VK_IMAGE_ASPECT_COLOR_BIT,
					VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

				// Copy region for transfer from framebuffer to cube face
				VkImageCopy copyRegion = {};

				copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				copyRegion.srcSubresource.baseArrayLayer = 0;
				copyRegion.srcSubresource.mipLevel = 0;
				copyRegion.srcSubresource.layerCount = 1;
				copyRegion.srcOffset = { 0, 0, 0 };

				copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				copyRegion.dstSubresource.baseArrayLayer = f;
				copyRegion.dstSubresource.mipLevel = m;
				copyRegion.dstSubresource.layerCount = 1;
				copyRegion.dstOffset = { 0, 0, 0 };

				copyRegion.extent.width = static_cast<uint32_t>(viewport.width);
				copyRegion.extent.height = static_cast<uint32_t>(viewport.height);
				copyRegion.extent.depth = 1;

				vkCmdCopyImage(
					cmdBuf,
					offscreen.image,
					VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					textures.irradianceCube.image,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					1,
					&copyRegion);

				// Transform framebuffer color attachment back
				vks::tools::setImageLayout(
					cmdBuf,
					offscreen.image,
					VK_IMAGE_ASPECT_COLOR_BIT,
					VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
			}
		}

		vks::tools::setImageLayout(
			cmdBuf,
			textures.irradianceCube.image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			subresourceRange);

		vulkanDevice->flushCommandBuffer(cmdBuf, queue);

		vkDestroyRenderPass(device, renderpass, nullptr);
		vkDestroyFramebuffer(device, offscreen.framebuffer, nullptr);
		vkFreeMemory(device, offscreen.memory, nullptr);
		vkDestroyImageView(device, offscreen.view, nullptr);
		vkDestroyImage(device, offscreen.image, nullptr);
		vkDestroyDescriptorPool(device, descriptorpool, nullptr);
		vkDestroyDescriptorSetLayout(device, descriptorsetlayout, nullptr);
		vkDestroyPipeline(device, pipeline, nullptr);
		vkDestroyPipelineLayout(device, pipelinelayout, nullptr);

		auto tEnd = std::chrono::high_resolution_clock::now();
		auto tDiff = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
		std::cout << "Generating irradiance cube with " << numMips << " mip levels took " << tDiff << " ms" << std::endl;
	}

	// Prefilter environment cubemap
	// See https://placeholderart.wordpress.com/2015/07/28/implementation-notes-runtime-environment-map-filtering-for-image-based-lighting/
	void generatePrefilteredCube()
	{
		auto tStart = std::chrono::high_resolution_clock::now();

		const VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
		const int32_t dim = 512;
		const uint32_t numMips = static_cast<uint32_t>(floor(log2(dim))) + 1;

		// Pre-filtered cube map
		// Image
		VkImageCreateInfo imageCI = vks::initializers::imageCreateInfo();
		imageCI.imageType = VK_IMAGE_TYPE_2D;
		imageCI.format = format;
		imageCI.extent.width = dim;
		imageCI.extent.height = dim;
		imageCI.extent.depth = 1;
		imageCI.mipLevels = numMips;
		imageCI.arrayLayers = 6;
		imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
		imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageCI.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		imageCI.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
		VK_CHECK_RESULT(vkCreateImage(device, &imageCI, nullptr, &textures.prefilteredCube.image));
		VkMemoryAllocateInfo memAlloc = vks::initializers::memoryAllocateInfo();
		VkMemoryRequirements memReqs;
		vkGetImageMemoryRequirements(device, textures.prefilteredCube.image, &memReqs);
		memAlloc.allocationSize = memReqs.size;
		memAlloc.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &textures.prefilteredCube.deviceMemory));
		VK_CHECK_RESULT(vkBindImageMemory(device, textures.prefilteredCube.image, textures.prefilteredCube.deviceMemory, 0));
		// Image view
		VkImageViewCreateInfo viewCI = vks::initializers::imageViewCreateInfo();
		viewCI.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
		viewCI.format = format;
		viewCI.subresourceRange = {};
		viewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewCI.subresourceRange.levelCount = numMips;
		viewCI.subresourceRange.layerCount = 6;
		viewCI.image = textures.prefilteredCube.image;
		VK_CHECK_RESULT(vkCreateImageView(device, &viewCI, nullptr, &textures.prefilteredCube.view));
		// Sampler
		VkSamplerCreateInfo samplerCI = vks::initializers::samplerCreateInfo();
		samplerCI.magFilter = VK_FILTER_LINEAR;
		samplerCI.minFilter = VK_FILTER_LINEAR;
		samplerCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerCI.minLod = 0.0f;
		samplerCI.maxLod = static_cast<float>(numMips);
		samplerCI.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
		VK_CHECK_RESULT(vkCreateSampler(device, &samplerCI, nullptr, &textures.prefilteredCube.sampler));

		textures.prefilteredCube.descriptor.imageView = textures.prefilteredCube.view;
		textures.prefilteredCube.descriptor.sampler = textures.prefilteredCube.sampler;
		textures.prefilteredCube.descriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		textures.prefilteredCube.device = vulkanDevice;

		// FB, Att, RP, Pipe, etc.
		VkAttachmentDescription attDesc = {};
		// Color attachment
		attDesc.format = format;
		attDesc.samples = VK_SAMPLE_COUNT_1_BIT;
		attDesc.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attDesc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attDesc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attDesc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attDesc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attDesc.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		VkAttachmentReference colorReference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

		VkSubpassDescription subpassDescription = {};
		subpassDescription.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpassDescription.colorAttachmentCount = 1;
		subpassDescription.pColorAttachments = &colorReference;

		// Use subpass dependencies for layout transitions
		std::array<VkSubpassDependency, 2> dependencies;
		dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
		dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
		dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
		dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
		dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

		// Renderpass
		VkRenderPassCreateInfo renderPassCI = vks::initializers::renderPassCreateInfo();
		renderPassCI.attachmentCount = 1;
		renderPassCI.pAttachments = &attDesc;
		renderPassCI.subpassCount = 1;
		renderPassCI.pSubpasses = &subpassDescription;
		renderPassCI.dependencyCount = 2;
		renderPassCI.pDependencies = dependencies.data();
		VkRenderPass renderpass;
		VK_CHECK_RESULT(vkCreateRenderPass(device, &renderPassCI, nullptr, &renderpass));

		struct {
			VkImage image;
			VkImageView view;
			VkDeviceMemory memory;
			VkFramebuffer framebuffer;
		} offscreen;

		// Offfscreen framebuffer
		{
			// Color attachment
			VkImageCreateInfo imageCreateInfo = vks::initializers::imageCreateInfo();
			imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
			imageCreateInfo.format = format;
			imageCreateInfo.extent.width = dim;
			imageCreateInfo.extent.height = dim;
			imageCreateInfo.extent.depth = 1;
			imageCreateInfo.mipLevels = 1;
			imageCreateInfo.arrayLayers = 1;
			imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
			imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
			imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			imageCreateInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VK_CHECK_RESULT(vkCreateImage(device, &imageCreateInfo, nullptr, &offscreen.image));

			VkMemoryAllocateInfo memAlloc = vks::initializers::memoryAllocateInfo();
			VkMemoryRequirements memReqs;
			vkGetImageMemoryRequirements(device, offscreen.image, &memReqs);
			memAlloc.allocationSize = memReqs.size;
			memAlloc.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &offscreen.memory));
			VK_CHECK_RESULT(vkBindImageMemory(device, offscreen.image, offscreen.memory, 0));

			VkImageViewCreateInfo colorImageView = vks::initializers::imageViewCreateInfo();
			colorImageView.viewType = VK_IMAGE_VIEW_TYPE_2D;
			colorImageView.format = format;
			colorImageView.flags = 0;
			colorImageView.subresourceRange = {};
			colorImageView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			colorImageView.subresourceRange.baseMipLevel = 0;
			colorImageView.subresourceRange.levelCount = 1;
			colorImageView.subresourceRange.baseArrayLayer = 0;
			colorImageView.subresourceRange.layerCount = 1;
			colorImageView.image = offscreen.image;
			VK_CHECK_RESULT(vkCreateImageView(device, &colorImageView, nullptr, &offscreen.view));

			VkFramebufferCreateInfo fbufCreateInfo = vks::initializers::framebufferCreateInfo();
			fbufCreateInfo.renderPass = renderpass;
			fbufCreateInfo.attachmentCount = 1;
			fbufCreateInfo.pAttachments = &offscreen.view;
			fbufCreateInfo.width = dim;
			fbufCreateInfo.height = dim;
			fbufCreateInfo.layers = 1;
			VK_CHECK_RESULT(vkCreateFramebuffer(device, &fbufCreateInfo, nullptr, &offscreen.framebuffer));

			VkCommandBuffer layoutCmd = vulkanDevice->createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);
			vks::tools::setImageLayout(
				layoutCmd,
				offscreen.image,
				VK_IMAGE_ASPECT_COLOR_BIT,
				VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
			vulkanDevice->flushCommandBuffer(layoutCmd, queue, true);
		}

		// Descriptors
		VkDescriptorSetLayout descriptorsetlayout;
		std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings = {
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 0),
		};
		VkDescriptorSetLayoutCreateInfo descriptorsetlayoutCI = vks::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings);
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorsetlayoutCI, nullptr, &descriptorsetlayout));

		// Descriptor Pool
		std::vector<VkDescriptorPoolSize> poolSizes = { vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1) };
		VkDescriptorPoolCreateInfo descriptorPoolCI = vks::initializers::descriptorPoolCreateInfo(poolSizes, 2);
		VkDescriptorPool descriptorpool;
		VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolCI, nullptr, &descriptorpool));

		// Descriptor sets
		VkDescriptorSet descriptorset;
		VkDescriptorSetAllocateInfo allocInfo = vks::initializers::descriptorSetAllocateInfo(descriptorpool, &descriptorsetlayout, 1);
		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &descriptorset));
		VkWriteDescriptorSet writeDescriptorSet = vks::initializers::writeDescriptorSet(descriptorset, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0, &textures.environmentCube.descriptor);
		vkUpdateDescriptorSets(device, 1, &writeDescriptorSet, 0, nullptr);

		// Pipeline layout
		struct PushBlock {
			glm::mat4 mvp;
			float roughness;
			uint32_t numSamples = 32u;
		} pushBlock;

		VkPipelineLayout pipelinelayout;
		std::vector<VkPushConstantRange> pushConstantRanges = {
			vks::initializers::pushConstantRange(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(PushBlock), 0),
		};
		VkPipelineLayoutCreateInfo pipelineLayoutCI = vks::initializers::pipelineLayoutCreateInfo(&descriptorsetlayout, 1);
		pipelineLayoutCI.pushConstantRangeCount = 1;
		pipelineLayoutCI.pPushConstantRanges = pushConstantRanges.data();
		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCI, nullptr, &pipelinelayout));

		// Pipeline
		VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = vks::initializers::pipelineInputAssemblyStateCreateInfo(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE);
		VkPipelineRasterizationStateCreateInfo rasterizationState = vks::initializers::pipelineRasterizationStateCreateInfo(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE);
		VkPipelineColorBlendAttachmentState blendAttachmentState = vks::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE);
		VkPipelineColorBlendStateCreateInfo colorBlendState = vks::initializers::pipelineColorBlendStateCreateInfo(1, &blendAttachmentState);
		VkPipelineDepthStencilStateCreateInfo depthStencilState = vks::initializers::pipelineDepthStencilStateCreateInfo(VK_FALSE, VK_FALSE, VK_COMPARE_OP_LESS_OR_EQUAL);
		VkPipelineViewportStateCreateInfo viewportState = vks::initializers::pipelineViewportStateCreateInfo(1, 1);
		VkPipelineMultisampleStateCreateInfo multisampleState = vks::initializers::pipelineMultisampleStateCreateInfo(VK_SAMPLE_COUNT_1_BIT);
		std::vector<VkDynamicState> dynamicStateEnables = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamicState = vks::initializers::pipelineDynamicStateCreateInfo(dynamicStateEnables);
		std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages;

		VkGraphicsPipelineCreateInfo pipelineCI = vks::initializers::pipelineCreateInfo(pipelinelayout, renderpass);
		pipelineCI.pInputAssemblyState = &inputAssemblyState;
		pipelineCI.pRasterizationState = &rasterizationState;
		pipelineCI.pColorBlendState = &colorBlendState;
		pipelineCI.pMultisampleState = &multisampleState;
		pipelineCI.pViewportState = &viewportState;
		pipelineCI.pDepthStencilState = &depthStencilState;
		pipelineCI.pDynamicState = &dynamicState;
		pipelineCI.stageCount = 2;
		pipelineCI.pStages = shaderStages.data();
		pipelineCI.renderPass = renderpass;
		pipelineCI.pVertexInputState = vkglTF::Vertex::getPipelineVertexInputState({ vkglTF::VertexComponent::Position, vkglTF::VertexComponent::Normal, vkglTF::VertexComponent::UV });

		shaderStages[0] = loadShader(getShadersPath() + "pbribl/filtercube.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		shaderStages[1] = loadShader(getShadersPath() + "pbribl/prefilterenvmap.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
		VkPipeline pipeline;
		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipeline));

		// Render

		VkClearValue clearValues[1];
		clearValues[0].color = { { 0.0f, 0.0f, 0.2f, 0.0f } };

		VkRenderPassBeginInfo renderPassBeginInfo = vks::initializers::renderPassBeginInfo();
		// Reuse render pass from example pass
		renderPassBeginInfo.renderPass = renderpass;
		renderPassBeginInfo.framebuffer = offscreen.framebuffer;
		renderPassBeginInfo.renderArea.extent.width = dim;
		renderPassBeginInfo.renderArea.extent.height = dim;
		renderPassBeginInfo.clearValueCount = 1;
		renderPassBeginInfo.pClearValues = clearValues;

		std::vector<glm::mat4> matrices = {
			// POSITIVE_X
			glm::rotate(glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f)), glm::radians(180.0f), glm::vec3(1.0f, 0.0f, 0.0f)),
			// NEGATIVE_X
			glm::rotate(glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(0.0f, 1.0f, 0.0f)), glm::radians(180.0f), glm::vec3(1.0f, 0.0f, 0.0f)),
			// POSITIVE_Y
			glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f)),
			// NEGATIVE_Y
			glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f)),
			// POSITIVE_Z
			glm::rotate(glm::mat4(1.0f), glm::radians(180.0f), glm::vec3(1.0f, 0.0f, 0.0f)),
			// NEGATIVE_Z
			glm::rotate(glm::mat4(1.0f), glm::radians(180.0f), glm::vec3(0.0f, 0.0f, 1.0f)),
		};

		VkCommandBuffer cmdBuf = vulkanDevice->createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);

		VkViewport viewport = vks::initializers::viewport((float)dim, (float)dim, 0.0f, 1.0f);
		VkRect2D scissor = vks::initializers::rect2D(dim, dim, 0, 0);

		vkCmdSetViewport(cmdBuf, 0, 1, &viewport);
		vkCmdSetScissor(cmdBuf, 0, 1, &scissor);

		VkImageSubresourceRange subresourceRange = {};
		subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		subresourceRange.baseMipLevel = 0;
		subresourceRange.levelCount = numMips;
		subresourceRange.layerCount = 6;

		// Change image layout for all cubemap faces to transfer destination
		vks::tools::setImageLayout(
			cmdBuf,
			textures.prefilteredCube.image,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			subresourceRange);

		for (uint32_t m = 0; m < numMips; m++) {
			pushBlock.roughness = (float)m / (float)(numMips - 1);
			for (uint32_t f = 0; f < 6; f++) {
				viewport.width = static_cast<float>(dim * std::pow(0.5f, m));
				viewport.height = static_cast<float>(dim * std::pow(0.5f, m));
				vkCmdSetViewport(cmdBuf, 0, 1, &viewport);

				// Render scene from cube face's point of view
				vkCmdBeginRenderPass(cmdBuf, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

				// Update shader push constant block
				pushBlock.mvp = glm::perspective((float)(M_PI / 2.0), 1.0f, 0.1f, 512.0f) * matrices[f];

				vkCmdPushConstants(cmdBuf, pipelinelayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushBlock), &pushBlock);

				vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
				vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelinelayout, 0, 1, &descriptorset, 0, NULL);

				models.skybox.draw(cmdBuf);

				vkCmdEndRenderPass(cmdBuf);

				vks::tools::setImageLayout(
					cmdBuf,
					offscreen.image,
					VK_IMAGE_ASPECT_COLOR_BIT,
					VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

				// Copy region for transfer from framebuffer to cube face
				VkImageCopy copyRegion = {};

				copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				copyRegion.srcSubresource.baseArrayLayer = 0;
				copyRegion.srcSubresource.mipLevel = 0;
				copyRegion.srcSubresource.layerCount = 1;
				copyRegion.srcOffset = { 0, 0, 0 };

				copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				copyRegion.dstSubresource.baseArrayLayer = f;
				copyRegion.dstSubresource.mipLevel = m;
				copyRegion.dstSubresource.layerCount = 1;
				copyRegion.dstOffset = { 0, 0, 0 };

				copyRegion.extent.width = static_cast<uint32_t>(viewport.width);
				copyRegion.extent.height = static_cast<uint32_t>(viewport.height);
				copyRegion.extent.depth = 1;

				vkCmdCopyImage(
					cmdBuf,
					offscreen.image,
					VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					textures.prefilteredCube.image,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					1,
					&copyRegion);

				// Transform framebuffer color attachment back
				vks::tools::setImageLayout(
					cmdBuf,
					offscreen.image,
					VK_IMAGE_ASPECT_COLOR_BIT,
					VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
			}
		}

		vks::tools::setImageLayout(
			cmdBuf,
			textures.prefilteredCube.image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			subresourceRange);

		vulkanDevice->flushCommandBuffer(cmdBuf, queue);

		vkDestroyRenderPass(device, renderpass, nullptr);
		vkDestroyFramebuffer(device, offscreen.framebuffer, nullptr);
		vkFreeMemory(device, offscreen.memory, nullptr);
		vkDestroyImageView(device, offscreen.view, nullptr);
		vkDestroyImage(device, offscreen.image, nullptr);
		vkDestroyDescriptorPool(device, descriptorpool, nullptr);
		vkDestroyDescriptorSetLayout(device, descriptorsetlayout, nullptr);
		vkDestroyPipeline(device, pipeline, nullptr);
		vkDestroyPipelineLayout(device, pipelinelayout, nullptr);

		auto tEnd = std::chrono::high_resolution_clock::now();
		auto tDiff = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
		std::cout << "Generating pre-filtered enivornment cube with " << numMips << " mip levels took " << tDiff << " ms" << std::endl;
	}

	// Prepare and initialize uniform buffer containing shader uniforms
	void prepareUniformBuffers()
	{
		// Object vertex shader uniform buffer
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&uniformBuffers.object,
			sizeof(uboMatrices)));

		// Skybox vertex shader uniform buffer
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&uniformBuffers.skybox,
			sizeof(uboMatrices)));

		// Shared parameter uniform buffer
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&uniformBuffers.params,
			sizeof(uboParams)));

		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&uniformBuffers.views,
			this->views.size() * sizeof(glm::mat4)));

		// Map persistent
		VK_CHECK_RESULT(uniformBuffers.object.map());
		VK_CHECK_RESULT(uniformBuffers.skybox.map());
		VK_CHECK_RESULT(uniformBuffers.params.map());
		VK_CHECK_RESULT(uniformBuffers.views.map());

		updateUniformBuffers();
		updateParams();
	}

	void updateUniformBuffers()
	{
		// 3D object
		uboMatrices.projection = camera.matrices.perspective;
		uboMatrices.view = camera.matrices.view;
		uboMatrices.model = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f + (models.objectIndex == 1 ? 45.0f : 0.0f)), glm::vec3(0.0f, 1.0f, 0.0f));
		uboMatrices.camPos = camera.position * -1.0f;
		memcpy(uniformBuffers.object.mapped, &uboMatrices, sizeof(uboMatrices));

		// Skybox
		uboMatrices.model = glm::mat4(glm::mat3(camera.matrices.view));
		memcpy(uniformBuffers.skybox.mapped, &uboMatrices, sizeof(uboMatrices));

		// Views
		for (size_t i = 0; i < this->views.size(); i++)
		{
			this->views[i] = camera.matrices.view;
		}

		memcpy(uniformBuffers.views.mapped, views.data(), this->views.size() * sizeof(glm::mat4));
	}

	void updateParams()
	{
		const float p = 15.0f;
		uboParams.lights[0] = glm::vec4(-p, -p * 0.5f, -p, 1.0f);
		uboParams.lights[1] = glm::vec4(-p, -p * 0.5f, p, 1.0f);
		uboParams.lights[2] = glm::vec4(p, -p * 0.5f, p, 1.0f);
		uboParams.lights[3] = glm::vec4(p, -p * 0.5f, -p, 1.0f);

		memcpy(uniformBuffers.params.mapped, &uboParams, this->views.size() * sizeof(glm::mat4));
	}

	bool checkMultiview()
	{
		VkPhysicalDeviceFeatures2KHR deviceFeatures2{};
		VkPhysicalDeviceMultiviewFeaturesKHR extFeatures{};
		extFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES_KHR;
		deviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR;
		deviceFeatures2.pNext = &extFeatures;
		PFN_vkGetPhysicalDeviceFeatures2KHR vkGetPhysicalDeviceFeatures2KHR = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2KHR>(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2KHR"));
		vkGetPhysicalDeviceFeatures2KHR(physicalDevice, &deviceFeatures2);
		std::cout << "Multiview features:" << std::endl;
		std::cout << "\tmultiview = " << extFeatures.multiview << std::endl;
		std::cout << "\tmultiviewGeometryShader = " << extFeatures.multiviewGeometryShader << std::endl;
		std::cout << "\tmultiviewTessellationShader = " << extFeatures.multiviewTessellationShader << std::endl;
		std::cout << std::endl;

		VkPhysicalDeviceProperties2KHR deviceProps2{};
		VkPhysicalDeviceMultiviewPropertiesKHR extProps{};
		extProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES_KHR;
		deviceProps2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR;
		deviceProps2.pNext = &extProps;
		PFN_vkGetPhysicalDeviceProperties2KHR vkGetPhysicalDeviceProperties2KHR = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2KHR>(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2KHR"));
		vkGetPhysicalDeviceProperties2KHR(physicalDevice, &deviceProps2);
		std::cout << "Multiview properties:" << std::endl;
		std::cout << "\tmaxMultiviewViewCount = " << extProps.maxMultiviewViewCount << std::endl;
		std::cout << "\tmaxMultiviewInstanceIndex = " << extProps.maxMultiviewInstanceIndex << std::endl;

		if (extFeatures.multiview == VK_TRUE && extProps.maxMultiviewViewCount > 0)
		{
			{
				physicalDeviceMultiviewProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES_KHR;
				physicalDeviceMultiviewProperties.pNext = nullptr;
				physicalDeviceMultiviewProperties.maxMultiviewViewCount = extProps.maxMultiviewViewCount;
				physicalDeviceMultiviewProperties.maxMultiviewInstanceIndex = extProps.maxMultiviewInstanceIndex;
			}

			return true;
		}

		return false;
	}

	bool prepareImGui()
	{
		{
			std::array<VkAttachmentDescription, 2> attachments = {};
			// Color attachment
			attachments[0].format = swapChain.colorFormat;
			attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
			attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
			attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachments[0].initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			// Depth attachment
			attachments[1].format = depthFormat;
			attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
			attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
			//attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			VkAttachmentReference colorReference = {};
			colorReference.attachment = 0;
			colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

			VkAttachmentReference depthReference = {};
			depthReference.attachment = 1;
			depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			VkSubpassDescription subpassDescription = {};
			subpassDescription.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpassDescription.colorAttachmentCount = 1;
			subpassDescription.pColorAttachments = &colorReference;
			subpassDescription.pDepthStencilAttachment = &depthReference;

			// Subpass dependencies for layout transitions
			std::array<VkSubpassDependency, 2> dependencies;

			dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[0].dstSubpass = 0;
			dependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
			dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

			dependencies[1].srcSubpass = 0;
			dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
			dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

			VkRenderPassCreateInfo renderPassCI{};
			renderPassCI.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
			renderPassCI.attachmentCount = static_cast<uint32_t>(attachments.size());
			renderPassCI.pAttachments = attachments.data();
			renderPassCI.subpassCount = 1;
			renderPassCI.pSubpasses = &subpassDescription;
			renderPassCI.dependencyCount = static_cast<uint32_t>(dependencies.size());
			renderPassCI.pDependencies = dependencies.data();

			VK_CHECK_RESULT(vkCreateRenderPass(device, &renderPassCI, nullptr, &imGuiPass.renderPass));

			return true;
		}
		return false;
	}

	bool prepareMultiview()
	{
		auto check_row_and_column = [&]()->bool
		{
			if (row > 0 && column > 0)
			{
				return true;
			}
			return false;
		};

		if (check_row_and_column())
		{
			std::size_t all_view_count = row * column;

			std::size_t frame_buffer_count = all_view_count / this->physicalDeviceMultiviewProperties.maxMultiviewViewCount;
			std::size_t remnant = all_view_count - frame_buffer_count * this->physicalDeviceMultiviewProperties.maxMultiviewViewCount;

			for (size_t frame_buffer_create_index = 0; frame_buffer_create_index < frame_buffer_count; frame_buffer_create_index++)
			{
				/*
					Layered depth/stencil framebuffer
				*/
				{
					MultiviewPass::FrameBufferAttachment depth_attachment = {};
					depth_attachment.arrayLayers = this->physicalDeviceMultiviewProperties.maxMultiviewViewCount;
					depth_attachment.usedArrayLayers = depth_attachment.arrayLayers;

					VkImageCreateInfo imageCI = vks::initializers::imageCreateInfo();
					imageCI.imageType = VK_IMAGE_TYPE_2D;
					imageCI.format = depthFormat;
					imageCI.extent = { width, height, 1 };
					imageCI.mipLevels = 1;
					imageCI.arrayLayers = depth_attachment.arrayLayers;
					imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
					imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
					imageCI.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
					imageCI.flags = 0;

					VK_CHECK_RESULT(vkCreateImage(device, &imageCI, nullptr, &depth_attachment.image));

					VkMemoryRequirements memReqs;
					vkGetImageMemoryRequirements(device, depth_attachment.image, &memReqs);

					VkMemoryAllocateInfo memAllocInfo{};
					memAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
					memAllocInfo.allocationSize = 0;
					memAllocInfo.memoryTypeIndex = 0;

					VkImageViewCreateInfo depthStencilViewCI = {};
					depthStencilViewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
					depthStencilViewCI.pNext = NULL;
					depthStencilViewCI.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
					depthStencilViewCI.format = depthFormat;
					depthStencilViewCI.flags = 0;
					depthStencilViewCI.subresourceRange = {};
					depthStencilViewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
					if (depthFormat >= VK_FORMAT_D16_UNORM_S8_UINT) {
						depthStencilViewCI.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
					}
					depthStencilViewCI.subresourceRange.baseMipLevel = 0;
					depthStencilViewCI.subresourceRange.levelCount = 1;
					depthStencilViewCI.subresourceRange.baseArrayLayer = 0;
					depthStencilViewCI.subresourceRange.layerCount = imageCI.arrayLayers;
					depthStencilViewCI.image = depth_attachment.image;

					memAllocInfo.allocationSize = memReqs.size;
					memAllocInfo.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
					VK_CHECK_RESULT(vkAllocateMemory(device, &memAllocInfo, nullptr, &depth_attachment.memory));
					VK_CHECK_RESULT(vkBindImageMemory(device, depth_attachment.image, depth_attachment.memory, 0));
					VK_CHECK_RESULT(vkCreateImageView(device, &depthStencilViewCI, nullptr, &depth_attachment.view));

					multiviewPass.depths.push_back(depth_attachment);
				}

				/*
					Layered color attachment
				*/
				{
					MultiviewPass::FrameBufferAttachment color_attachment = {};
					color_attachment.arrayLayers = this->physicalDeviceMultiviewProperties.maxMultiviewViewCount;
					color_attachment.usedArrayLayers = color_attachment.arrayLayers;

					VkImageCreateInfo imageCI = vks::initializers::imageCreateInfo();
					imageCI.imageType = VK_IMAGE_TYPE_2D;
					imageCI.format = swapChain.colorFormat;
					imageCI.extent = { width, height, 1 };
					imageCI.mipLevels = 1;
					imageCI.arrayLayers = color_attachment.arrayLayers;
					imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
					imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
					imageCI.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
					VK_CHECK_RESULT(vkCreateImage(device, &imageCI, nullptr, &color_attachment.image));

					VkMemoryRequirements memReqs;
					vkGetImageMemoryRequirements(device, color_attachment.image, &memReqs);

					VkMemoryAllocateInfo memoryAllocInfo = vks::initializers::memoryAllocateInfo();
					memoryAllocInfo.allocationSize = memReqs.size;
					memoryAllocInfo.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
					VK_CHECK_RESULT(vkAllocateMemory(device, &memoryAllocInfo, nullptr, &color_attachment.memory));
					VK_CHECK_RESULT(vkBindImageMemory(device, color_attachment.image, color_attachment.memory, 0));

					VkImageViewCreateInfo imageViewCI = vks::initializers::imageViewCreateInfo();
					imageViewCI.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
					imageViewCI.format = swapChain.colorFormat;
					imageViewCI.flags = 0;
					imageViewCI.subresourceRange = {};
					imageViewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
					imageViewCI.subresourceRange.baseMipLevel = 0;
					imageViewCI.subresourceRange.levelCount = 1;
					imageViewCI.subresourceRange.baseArrayLayer = 0;
					imageViewCI.subresourceRange.layerCount = imageCI.arrayLayers;
					imageViewCI.image = color_attachment.image;
					VK_CHECK_RESULT(vkCreateImageView(device, &imageViewCI, nullptr, &color_attachment.view));

					multiviewPass.colors.push_back(color_attachment);
				}
			}

			if (remnant > 0)
			{
				/*
					Layered depth/stencil framebuffer
				*/
				{
					MultiviewPass::FrameBufferAttachment depth_attachment = {};
					depth_attachment.arrayLayers = this->physicalDeviceMultiviewProperties.maxMultiviewViewCount;
					depth_attachment.usedArrayLayers = remnant;

					VkImageCreateInfo imageCI = vks::initializers::imageCreateInfo();
					imageCI.imageType = VK_IMAGE_TYPE_2D;
					imageCI.format = depthFormat;
					imageCI.extent = { width, height, 1 };
					imageCI.mipLevels = 1;
					imageCI.arrayLayers = depth_attachment.arrayLayers;
					imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
					imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
					imageCI.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
					imageCI.flags = 0;

					VK_CHECK_RESULT(vkCreateImage(device, &imageCI, nullptr, &depth_attachment.image));

					VkMemoryRequirements memReqs;
					vkGetImageMemoryRequirements(device, depth_attachment.image, &memReqs);

					VkMemoryAllocateInfo memAllocInfo{};
					memAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
					memAllocInfo.allocationSize = 0;
					memAllocInfo.memoryTypeIndex = 0;

					VkImageViewCreateInfo depthStencilViewCI = {};
					depthStencilViewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
					depthStencilViewCI.pNext = NULL;
					depthStencilViewCI.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
					depthStencilViewCI.format = depthFormat;
					depthStencilViewCI.flags = 0;
					depthStencilViewCI.subresourceRange = {};
					depthStencilViewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
					if (depthFormat >= VK_FORMAT_D16_UNORM_S8_UINT) {
						depthStencilViewCI.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
					}
					depthStencilViewCI.subresourceRange.baseMipLevel = 0;
					depthStencilViewCI.subresourceRange.levelCount = 1;
					depthStencilViewCI.subresourceRange.baseArrayLayer = 0;
					depthStencilViewCI.subresourceRange.layerCount = imageCI.arrayLayers;
					depthStencilViewCI.image = depth_attachment.image;

					memAllocInfo.allocationSize = memReqs.size;
					memAllocInfo.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
					VK_CHECK_RESULT(vkAllocateMemory(device, &memAllocInfo, nullptr, &depth_attachment.memory));
					VK_CHECK_RESULT(vkBindImageMemory(device, depth_attachment.image, depth_attachment.memory, 0));
					VK_CHECK_RESULT(vkCreateImageView(device, &depthStencilViewCI, nullptr, &depth_attachment.view));

					multiviewPass.depths.push_back(depth_attachment);
				}

				/*
					Layered color attachment
				*/
				{
					MultiviewPass::FrameBufferAttachment color_attachment = {};
					color_attachment.arrayLayers = this->physicalDeviceMultiviewProperties.maxMultiviewViewCount;
					color_attachment.usedArrayLayers = remnant;

					VkImageCreateInfo imageCI = vks::initializers::imageCreateInfo();
					imageCI.imageType = VK_IMAGE_TYPE_2D;
					imageCI.format = swapChain.colorFormat;
					imageCI.extent = { width, height, 1 };
					imageCI.mipLevels = 1;
					imageCI.arrayLayers = color_attachment.arrayLayers;
					imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
					imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
					imageCI.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
					VK_CHECK_RESULT(vkCreateImage(device, &imageCI, nullptr, &color_attachment.image));

					VkMemoryRequirements memReqs;
					vkGetImageMemoryRequirements(device, color_attachment.image, &memReqs);

					VkMemoryAllocateInfo memoryAllocInfo = vks::initializers::memoryAllocateInfo();
					memoryAllocInfo.allocationSize = memReqs.size;
					memoryAllocInfo.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
					VK_CHECK_RESULT(vkAllocateMemory(device, &memoryAllocInfo, nullptr, &color_attachment.memory));
					VK_CHECK_RESULT(vkBindImageMemory(device, color_attachment.image, color_attachment.memory, 0));

					VkImageViewCreateInfo imageViewCI = vks::initializers::imageViewCreateInfo();
					imageViewCI.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
					imageViewCI.format = swapChain.colorFormat;
					imageViewCI.flags = 0;
					imageViewCI.subresourceRange = {};
					imageViewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
					imageViewCI.subresourceRange.baseMipLevel = 0;
					imageViewCI.subresourceRange.levelCount = 1;
					imageViewCI.subresourceRange.baseArrayLayer = 0;
					imageViewCI.subresourceRange.layerCount = imageCI.arrayLayers;
					imageViewCI.image = color_attachment.image;
					VK_CHECK_RESULT(vkCreateImageView(device, &imageViewCI, nullptr, &color_attachment.view));

					multiviewPass.colors.push_back(color_attachment);
				}
			}

			/*
				Renderpass
			*/
			{}
			{
				std::array<VkAttachmentDescription, 2> attachments = {};
				// Color attachment
				attachments[0].format = swapChain.colorFormat;
				attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
				attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
				attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
				attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
				attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				// Depth attachment
				attachments[1].format = depthFormat;
				attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
				attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
				attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
				attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
				attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

				VkAttachmentReference colorReference = {};
				colorReference.attachment = 0;
				colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

				VkAttachmentReference depthReference = {};
				depthReference.attachment = 1;
				depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

				VkSubpassDescription subpassDescription = {};
				subpassDescription.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
				subpassDescription.colorAttachmentCount = 1;
				subpassDescription.pColorAttachments = &colorReference;
				subpassDescription.pDepthStencilAttachment = &depthReference;

				// Subpass dependencies for layout transitions
				std::array<VkSubpassDependency, 2> dependencies;

				dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
				dependencies[0].dstSubpass = 0;
				dependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
				dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
				dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
				dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

				dependencies[1].srcSubpass = 0;
				dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
				dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
				dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
				dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
				dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

				VkRenderPassCreateInfo renderPassCI{};
				renderPassCI.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
				renderPassCI.attachmentCount = static_cast<uint32_t>(attachments.size());
				renderPassCI.pAttachments = attachments.data();
				renderPassCI.subpassCount = 1;
				renderPassCI.pSubpasses = &subpassDescription;
				renderPassCI.dependencyCount = static_cast<uint32_t>(dependencies.size());
				renderPassCI.pDependencies = dependencies.data();

				/*
					Setup multiview info for the renderpass
				*/
				uint32_t viewMask = 0;
				for (size_t bit_index = 0; bit_index < this->physicalDeviceMultiviewProperties.maxMultiviewViewCount; bit_index++)
				{
					viewMask |= (uint32_t)std::pow(2, bit_index);
				}

				/*
					Bit mask that specifies correlation between views
					An implementation may use this for optimizations (concurrent render)
				*/
				uint32_t correlationMask = viewMask;

				VkRenderPassMultiviewCreateInfo renderPassMultiviewCI{};
				renderPassMultiviewCI.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
				renderPassMultiviewCI.subpassCount = 1;
				renderPassMultiviewCI.pViewMasks = &viewMask;
				renderPassMultiviewCI.correlationMaskCount = 1;
				renderPassMultiviewCI.pCorrelationMasks = &correlationMask;

				renderPassCI.pNext = &renderPassMultiviewCI;

				VK_CHECK_RESULT(vkCreateRenderPass(device, &renderPassCI, nullptr, &multiviewPass.renderPass));
			}

			/*
				Framebuffer
			*/
			{
				for (size_t frame_buffer_index = 0; frame_buffer_index < multiviewPass.colors.size(); frame_buffer_index++)
				{
					VkFramebuffer multiview_framebuffer = VK_NULL_HANDLE;

					VkImageView attachments[2];
					attachments[0] = multiviewPass.colors[frame_buffer_index].view;
					attachments[1] = multiviewPass.depths[frame_buffer_index].view;

					VkFramebufferCreateInfo framebufferCI = vks::initializers::framebufferCreateInfo();
					framebufferCI.renderPass = multiviewPass.renderPass;
					framebufferCI.attachmentCount = 2;
					framebufferCI.pAttachments = attachments;
					framebufferCI.width = width;
					framebufferCI.height = height;
					framebufferCI.layers = 1;
					VK_CHECK_RESULT(vkCreateFramebuffer(device, &framebufferCI, nullptr, &multiview_framebuffer));

					multiviewPass.frameBuffers.push_back(multiview_framebuffer);
				}
			}

			return true;
		}

		return false;
	}

	virtual void windowResized()
	{
		vkDestroyRenderPass(device, multiviewPass.renderPass, nullptr);

		for (auto& item : multiviewPass.frameBuffers)
		{
			vkDestroyFramebuffer(device, item, nullptr);
		}

		for (auto& item : multiviewPass.colors)
		{
			vkDestroyImageView(device, item.view, nullptr);
			vkDestroyImage(device, item.image, nullptr);
			vkFreeMemory(device, item.memory, nullptr);
		}

		for (auto& item : multiviewPass.depths)
		{
			vkDestroyImageView(device, item.view, nullptr);
			vkDestroyImage(device, item.image, nullptr);
			vkFreeMemory(device, item.memory, nullptr);
		}

		multiviewPass.colors.clear();
		multiviewPass.depths.clear();
		multiviewPass.frameBuffers.clear();
		multiviewPass.renderPass = VK_NULL_HANDLE;

		prepareMultiview();

		resized = false;
		buildCommandBuffers();
	}

	void prepare()
	{
		VulkanExampleBase::prepare();

		prepareImGui();

		{
			if (!checkMultiview())
			{
				vks::tools::exitFatal("Could not support multiview : \n" + vks::tools::errorString(VkResult::VK_ERROR_FEATURE_NOT_PRESENT), VkResult::VK_ERROR_FEATURE_NOT_PRESENT);
				return;
			}
			else
			{
				if (!prepareMultiview())
				{
					vks::tools::exitFatal("Could not prepare multiview : \n" + vks::tools::errorString(VkResult::VK_ERROR_INITIALIZATION_FAILED), VkResult::VK_ERROR_INITIALIZATION_FAILED);
					return;
				}
			}
		}

		loadAssets();
		generateBRDFLUT();
		generateIrradianceCube();
		generatePrefilteredCube();
		prepareUniformBuffers();
		setupDescriptors();
		preparePipelines();
		buildCommandBuffers();
		prepared = true;
	}


	virtual void render()
	{
		if (!prepared)
			return;
		updateUniformBuffers();
		draw();
	}

	void draw()
	{
		VulkanExampleBase::prepareFrame();
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &drawCmdBuffers[currentBuffer];
		VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));
		VulkanExampleBase::submitFrame();
	}

	virtual void OnUpdateUIOverlay(vks::UIOverlay* overlay)
	{
		if (overlay->header("Settings")) {
			if (overlay->comboBox("Material", &materialIndex, materialNames)) {
				buildCommandBuffers();
			}
			if (overlay->comboBox("Object type", &models.objectIndex, objectNames)) {
				updateUniformBuffers();
				buildCommandBuffers();
			}
			if (overlay->inputFloat("Exposure", &uboParams.exposure, 0.1f, 2)) {
				updateParams();
			}
			if (overlay->inputFloat("Gamma", &uboParams.gamma, 0.1f, 2)) {
				updateParams();
			}
			if (overlay->checkBox("Skybox", &displaySkybox)) {
				buildCommandBuffers();
			}
		}
	}

};

VULKAN_EXAMPLE_MAIN()
